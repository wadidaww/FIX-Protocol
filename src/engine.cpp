// =============================================================================
// FIX Protocol Engine - Engine implementation
// =============================================================================
#include "fix/engine.hpp"

#include <algorithm>
#include <stdexcept>

namespace fix {

namespace {

// Intentional leak, see Engine::stop(): a transport whose IO thread is the
// current thread can never be joined or destroyed on this thread (~thread on
// a joinable std::thread aborts the process). Parking the entry in a heap
// allocation that is deliberately never freed keeps the joinable thread object
// alive for the rest of the process' life; stop() has already requested
// shutdown, so the IO thread exits on its own.
template <typename T>
void quarantine(T entry) {
    static auto *sink = new std::vector<T>; // never deleted – on purpose
    sink->push_back(std::move(entry));
}

// Invoke the engine error sink from an exception backstop. The sink is user
// code and may itself throw, so every path here is guarded: a throwing sink
// must not take the timer/IO/shutdown thread down with it. noexcept on
// purpose – this is the last line of defence.
void report_error(const std::function<void(std::error_code)> &sink, std::error_code ec) noexcept {
    if (!sink)
        return;
    try {
        sink(ec);
    } catch (...) {
        // Nothing sensible left to report to; swallow so the caller survives.
    }
}

} // namespace

Engine::Engine(EngineConfig cfg)
    : cfg_(std::move(cfg)) {
    if (cfg_.enable_audit) {
        FileAuditLog::Config ac;
        ac.dir = cfg_.log_dir;
        ac.base_name = "fix_audit";
        audit_log_ = std::make_unique<FileAuditLog>(std::move(ac));
    } else {
        audit_log_ = std::make_unique<NullAuditLog>();
    }
}

Engine::~Engine() {
    stop();
}

Result<void> Engine::start() {
    if (running_.load())
        return make_unexpected(ErrorCode::SessionError);
    running_.store(true, std::memory_order_release);

    timer_thread_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.timer_interval_ms));
            // Backstop (2.6): tick each session behind an exception boundary —
            // Session::on_timer reaches user callbacks (on_heartbeat_timeout),
            // do_send and do_disconnect with no catch of its own, and Session
            // only catches std::exception per *inbound message* deeper down.
            // A throw here (std or not) must not unwind out of the thread
            // entry (std::terminate) nor starve the other sessions: catch per
            // session, keep ticking, report afterwards. Reporting is deferred
            // until after for_each() returns so the sink never runs while
            // SessionManager's shared_lock is held (a sink calling
            // add/remove_session would deadlock on that same mutex).
            std::error_code cb_error;
            try {
                sessions_.for_each([&cb_error](Session &s) {
                    try {
                        s.on_timer();
                    } catch (const std::exception &) {
                        cb_error = make_error_code(ErrorCode::SessionError);
                    } catch (...) {
                        cb_error = make_error_code(ErrorCode::SessionError);
                    }
                });
                // Sessions removed from a session callback (their own IO
                // thread) are reaped here: join the IO thread, then destroy –
                // never on the IO thread itself (that would ~thread-abort).
                drain_reap_list();
            } catch (...) {
                cb_error = make_error_code(ErrorCode::SessionError);
            }
            if (cb_error)
                report_error(cfg_.on_error, cb_error);
        }
    });

    // Snapshot under the lock, start outside it: start() may join a stale
    // thread left by an earlier stop()-from-callback, and that thread could be
    // inside a session callback calling Engine::remove_session() (which takes
    // conn_mutex_) – holding the lock across start() deadlocks.
    std::vector<std::shared_ptr<ITransport>> transports;
    {
        std::lock_guard lock(conn_mutex_);
        transports.reserve(connections_.size());
        for (const auto &conn : connections_)
            transports.push_back(conn.transport);
    }
    for (const auto &transport : transports)
        (void)transport->start();

    return {};
}

void Engine::stop() {
    if (!running_.exchange(false))
        return;
    if (timer_thread_.joinable()) {
        if (timer_thread_.get_id() == std::this_thread::get_id() || on_transport_io_thread())
            // Called from the timer callback itself (joining self aborts) or
            // from a transport IO thread (the timer may currently be joining
            // *this* thread in drain_reap_list() – joining it back would
            // deadlock). Detach instead: the loop exits on its next iteration
            // because running_ is already false. stop()-from-a-callback is a
            // documented contract violation anyway; the quarantine below is
            // the backstop, not a supported path.
            timer_thread_.detach();
        else
            timer_thread_.join();
    }

    // Logout all sessions. Backstop (2.6): logout() runs do_send and may reach
    // user callbacks (a peer's Logout echo invokes on_logout on its IO thread;
    // session changes may invoke it inline) — Session only catches
    // std::exception per inbound message deeper down, so a throw here would
    // abort Engine::stop() halfway and leave IO threads unjoined (wedged
    // process). Catch per session, never rethrow, report after for_each()
    // returns so the sink runs without SessionManager's shared_lock held.
    std::error_code cb_error;
    try {
        sessions_.for_each([&cb_error](Session &s) {
            if (!s.is_active())
                return;
            try {
                s.logout("Engine shutdown");
            } catch (const std::exception &) {
                cb_error = make_error_code(ErrorCode::SessionError);
            } catch (...) {
                cb_error = make_error_code(ErrorCode::SessionError);
            }
        });
    } catch (...) {
        cb_error = make_error_code(ErrorCode::SessionError);
    }
    if (cb_error)
        report_error(cfg_.on_error, cb_error);

    // Snapshot under the lock, stop outside it: stop() joins the transport IO
    // threads, and a callback running on one of them may re-enter the Engine
    // (remove_session/stop) which takes conn_mutex_ – holding it across the
    // join deadlocks (reproduced).
    std::vector<Connection> conns;
    {
        std::lock_guard lock(conn_mutex_);
        conns = std::move(connections_);
        connections_.clear(); // moved-from state must not be relied upon
    }
    for (auto &conn : conns) {
        const bool self = conn.transport->on_io_thread();
        conn.transport->stop(); // self: requests shutdown but cannot join
        if (self) {
            // This IO thread *is* us: park the entry for the reap pass below,
            // which quarantines anything still owned by this thread.
            std::lock_guard lock(conn_mutex_);
            reap_list_.push_back(std::move(conn));
        }
        // else: joined here; conn is released at the end of the iteration,
        // i.e. the session is destroyed only after its IO thread is gone.
    }

    // The timer thread is gone, so nothing else drains the reap list: do a
    // synchronous pass now (joins everything not owned by this thread), then
    // quarantine what remains.
    drain_reap_list();
}

void Engine::drain_reap_list() {
    std::vector<Connection> reap;
    {
        std::lock_guard lock(conn_mutex_);
        reap.swap(reap_list_);
    }
    // Join outside the lock: the IO thread being joined may itself call
    // remove_session()/stop(), both of which take conn_mutex_.
    for (auto &conn : reap) {
        if (conn.transport->on_io_thread()) {
            // Owned by this thread – stop() only requests shutdown here;
            // destroying the joinable thread object on its own thread would
            // abort, so park it in the quarantine instead.
            conn.transport->stop();
            quarantine(std::move(conn));
        } else {
            conn.transport->stop(); // joins; conn released after the join
        }
    }
}

Session *Engine::add_session(SessionConfig cfg, std::unique_ptr<ITransport> transport,
                             SessionCallbacks user_cbs, const DataDictionary *dict) {
    if (!transport)
        return nullptr;

    // The transport is shared: the session's do_send/do_disconnect closures,
    // the Connection and the (weak) transport callbacks all need to keep it
    // alive, and stop() must be able to join its IO thread before anything is
    // destroyed.
    auto shared_transport = std::shared_ptr<ITransport>(std::move(transport));

    // Wire session → transport *before* the session is created: the closures
    // only capture the transport, which already exists at this point.
    SessionCallbacks internal_cbs = std::move(user_cbs);
    internal_cbs.do_send = [t = shared_transport, sink = cfg_.on_error](const std::string &bytes) {
        // 2.6: the transport Result used to be discarded ((void) cast) — a
        // failed write (peer gone, EPIPE, torn-down socket) vanished without a
        // trace. Surface it on the engine error sink; error_code already
        // carries the fix::ErrorCode message.
        auto sent = t->send(bytes);
        if (!sent)
            report_error(sink, sent.error());
    };
    // Session::disconnect() (CompID mismatch, heartbeat timeout, …) must
    // actually close the socket, otherwise a hostile peer pins the acceptor's
    // event loop forever and initiators never reconnect (F1).
    internal_cbs.do_disconnect = [t = shared_transport] {
        t->disconnect();
    };

    // Create store
    std::unique_ptr<IMessageStore> store;
    if (cfg_.use_file_store) {
        store = std::make_unique<FileStore>(cfg_.store_dir, cfg.id);
    } else {
        store = std::make_unique<MemoryStore>();
    }

    auto session =
        sessions_.create_session(std::move(cfg), std::move(store), dict, std::move(internal_cbs));
    if (!session)
        return nullptr; // duplicate SessionID – never silently replace

    // Wire transport → session. Callbacks capture a weak_ptr: a session that is
    // removed while the transport IO thread is still draining cannot be
    // touched after destruction. Each callback is an exception backstop (2.6):
    // they run on the transport's IO thread, Session only catches
    // std::exception per inbound message deeper down, and an exception
    // escaping into the transport event loop would terminate the IO thread —
    // catch (std and non-std), report on the engine sink, keep the loop alive.
    // The sink is captured by value (not `this`): a transport may be parked in
    // Engine's never-destroyed quarantine and outlive the Engine itself.
    std::weak_ptr<Session> weak = session;
    shared_transport->set_on_connected([weak, sink = cfg_.on_error] {
        try {
            if (auto s = weak.lock())
                s->on_transport_connected();
        } catch (const std::exception &) {
            report_error(sink, make_error_code(ErrorCode::SessionError));
        } catch (...) {
            report_error(sink, make_error_code(ErrorCode::SessionError));
        }
    });
    shared_transport->set_on_data([weak, sink = cfg_.on_error](const char *data, std::size_t len) {
        try {
            if (auto s = weak.lock())
                s->on_data(data, len);
        } catch (const std::exception &) {
            report_error(sink, make_error_code(ErrorCode::SessionError));
        } catch (...) {
            report_error(sink, make_error_code(ErrorCode::SessionError));
        }
    });
    shared_transport->set_on_disconnected(
        [weak, sink = cfg_.on_error](std::string_view /*reason*/) {
            try {
                if (auto s = weak.lock())
                    s->disconnect();
            } catch (const std::exception &) {
                report_error(sink, make_error_code(ErrorCode::SessionError));
            } catch (...) {
                report_error(sink, make_error_code(ErrorCode::SessionError));
            }
        });
    // Transport errors used to be fired into the void (set_on_error was never
    // called): surface them on the engine-level sink instead (F6). Backstop:
    // the sink is user code invoked from deep inside the transport event loop
    // — a throw there would take the IO thread down, so swallow it (the sink
    // already failed once; there is nothing else to report to).
    shared_transport->set_on_error(
        [sink = cfg_.on_error](std::error_code ec) { report_error(sink, ec); });

    bool should_start = false;
    {
        std::lock_guard lock(conn_mutex_);
        connections_.push_back({shared_transport, session});
        // add_session() after start(): start the transport below – Engine::
        // start() only sweeps the connections that existed when it ran.
        should_start = running_.load(std::memory_order_acquire);
    }
    if (should_start) {
        // Start *outside* conn_mutex_: start() may join a stale thread left
        // by an earlier stop()-from-callback, and that thread could be inside
        // a session callback that calls Engine::remove_session() (which takes
        // conn_mutex_) – holding the lock across start() deadlocks (F4).
        if (!shared_transport->start().has_value()) {
            // Roll back under a brief lock re-acquire, then stop outside it.
            bool owned = false;
            {
                std::lock_guard lock(conn_mutex_);
                auto it = std::find_if(connections_.begin(), connections_.end(),
                                       [&shared_transport](const Connection &c) {
                                           return c.transport == shared_transport;
                                       });
                if (it != connections_.end()) {
                    connections_.erase(it);
                    owned = true;
                }
            }
            if (owned)
                shared_transport->stop();
            (void)sessions_.remove(session->id());
            return nullptr;
        }
    }

    return session.get();
}

bool Engine::remove_session(const SessionID &sid) {
    Connection conn;
    bool found = false;
    {
        std::lock_guard lock(conn_mutex_);
        auto it =
            std::find_if(connections_.begin(), connections_.end(), [&sid](const Connection &c) {
                return c.session && c.session->id() == sid;
            });
        if (it != connections_.end()) {
            // Drop the connection first so a concurrent Engine::start()/stop()
            // cannot resurrect it; the local Connection keeps both alive.
            conn = std::move(*it);
            connections_.erase(it);
            found = true;
        }
    }
    // Remove from the manager immediately so lookups fail from here on. The
    // local Connection (or the reap entry below) keeps the session alive until
    // its transport IO thread has been joined.
    const bool removed = sessions_.remove(sid);

    if (found) {
        if (conn.transport->on_io_thread()) {
            // Called from this transport's own IO thread (a session callback):
            // stop() cannot join itself, and destroying the joinable thread
            // here would abort (~thread). Park the entry for the timer thread,
            // which reaps it off this thread (F3). sessions_.remove above
            // already ran, so lookups fail while the reap entry holds the last
            // strong reference.
            std::lock_guard lock(conn_mutex_);
            reap_list_.push_back(std::move(conn));
        } else {
            // Stop + join the IO thread *outside* conn_mutex_ (never hold it
            // across a join – F4); the session is destroyed after the join
            // when `conn` goes out of scope.
            conn.transport->stop();
        }
    }
    return removed;
}

Session *Engine::get_session(const SessionID &sid) noexcept {
    return sessions_.find(sid);
}

void Engine::load_dictionary(FixVersion v, const std::filesystem::path &xml_path) {
    auto dict = std::make_shared<DataDictionary>();
    dict->load_builtin(v);
    dict->load(xml_path); // overlay with XML if available
    DictionaryRegistry::instance().set(v, std::move(dict));
}

void Engine::load_builtin_dictionary(FixVersion v) {
    auto dict = std::make_shared<DataDictionary>();
    dict->load_builtin(v);
    DictionaryRegistry::instance().set(v, std::move(dict));
}

const DataDictionary *Engine::dictionary(FixVersion v) const noexcept {
    return DictionaryRegistry::instance().get(v);
}

} // namespace fix
