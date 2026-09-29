#pragma once
// =============================================================================
// FIX Protocol Engine - TCP Transport (epoll-based, Linux/POSIX)
// =============================================================================
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "transport.hpp"

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fix {

class TcpTransport : public ITransport {
public:
    // Keepalive tuning applied to EVERY connection socket — accepted *and*
    // initiated (3.3): SO_KEEPALIVE plus the TCP-level idle/interval/count
    // knobs where the platform spells them TCP_KEEPIDLE/KEEPINTVL/KEEPCNT
    // (macOS/BSD: TCP_KEEPALIVE for the idle time). Backstop only: FIX
    // heartbeats detect a dead peer far faster; TCP keepalive catches
    // half-open peers whose FIN/ACKs were swallowed (NAT idle expiry, cable
    // pull, crashed host) so the socket eventually errors out instead of
    // staying "connected" forever.
    static constexpr int kKeepIdleSec = 30;
    static constexpr int kKeepIntvlSec = 10;
    static constexpr int kKeepCnt = 6;

    // Diagnostic snapshot of the LIVE connection's socket options (used by
    // tests to assert post-connect option wiring; ops can log it). Reads the
    // fd under send_mutex_, which also excludes close_connection(), so the fd
    // cannot be closed underneath the getsockopt calls. Fails when no
    // connection is established.
    struct SocketOptions {
        bool keepalive = false;
        bool nodelay = false;
        int keepidle_sec = 0;
        int keepintvl_sec = 0;
        int keepcnt = 0;
        int send_buffer_bytes = 0; // kernel-reported SO_SNDBUF
    };
    Result<SocketOptions> socket_options() const;

    explicit TcpTransport(TcpTransportConfig cfg);
    ~TcpTransport() override;

    void set_on_connected(ConnectedCallback cb) override { on_connected_ = std::move(cb); }
    void set_on_disconnected(DisconnectedCallback cb) override { on_disconnected_ = std::move(cb); }
    void set_on_data(DataCallback cb) override { on_data_ = std::move(cb); }
    void set_on_error(ErrorCallback cb) override { on_error_ = std::move(cb); }

    Result<void> start() override;
    void stop() override;
    void disconnect() override;
    // Re-export the base overloads: declaring send(char*, size_t) here would
    // otherwise hide ITransport::send(const std::string&) for callers holding
    // the concrete type (name hiding).
    using ITransport::send;
    Result<void> send(const char *data, std::size_t len) override;
    [[nodiscard]] bool is_connected() const noexcept override {
        return connected_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool on_io_thread() const noexcept override {
        return detail::current_transport_io == this;
    }

private:
    TcpTransportConfig cfg_;
    ConnectedCallback on_connected_;
    DisconnectedCallback on_disconnected_;
    DataCallback on_data_;
    ErrorCallback on_error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    // Serialises start() against itself (stale-thread join ↔ running_ re-arm).
    // start()/stop() must still not be called concurrently with each other —
    // the same contract as before; only concurrent start() calls are made safe.
    std::mutex lifecycle_mutex_;
    // Set by disconnect() while a connection is live; the IO thread's event
    // loop observes it, tears the connection down (which clears it) and
    // continues its outer loop (acceptor → accept, initiator → reconnect).
    std::atomic<bool> disconnect_requested_{false};
    std::thread io_thread_;

    // Socket handles. listen_fd_ / conn_fd_ are owned by the IO thread (they are
    // only ever created, used and closed there); stop() never touches them so it
    // can never race the IO thread on a file descriptor number. conn_fd_ is
    // guarded by send_mutex_ because send() reads it from caller threads.
    int listen_fd_ = -1;
    int conn_fd_ = -1;

    // Self-pipe used to interrupt epoll_wait()/poll() from stop(), from a
    // cross-thread send() that queued data (3.3 flush kick) and to make the
    // reconnect backoff sleep interruptible. Created in start(), reused across
    // restart cycles and closed only in the destructor: wake() may be called
    // from arbitrary threads (disconnect()/send()) that could still hold a
    // stale copy of the fd, so stop() must not close it (fd-reuse hazard).
    // Both ends staying open also means writing to it can never raise SIGPIPE.
    int wake_pipe_[2] = {-1, -1};

    // Send queue. Leaf lock (see the lock order in session_manager.hpp): the
    // enqueue path may be reached while Session::send_mutex_ is held (the
    // accepted do_send boundary) but never acquires another engine lock and
    // never runs user code while locked — handle_send()/socket_options()
    // release it before firing callbacks or returning.
    mutable std::mutex send_mutex_;
    std::deque<std::string> send_queue_;
    std::vector<char> recv_buf_;

    // Cached DNS result (3.3): resolved once per start() on the CALLER's
    // thread (resolve_target), read by the IO thread for every connect
    // attempt, freed only when the previous IO thread has been joined (start)
    // or in the destructor (stop). Reconnects never call getaddrinfo again.
    struct addrinfo *resolved_ = nullptr;

    // -- Threads --------------------------------------------------------------
    void run_acceptor();
    void run_initiator();

    // Serve one connection until it dies. Returns the disconnect reason
    // (empty string => the transport was stopped).
    std::string_view run_event_loop(int fd);

    // Non-blocking connect using the address list cached by resolve_target()
    // (incl. wait-for-writable + SO_ERROR verification). Returns true once a
    // connection has been established (the event loop runs to completion
    // before returning).
    bool do_connect();

    // Resolve cfg_.host:cfg_.port once per start(), on the caller's thread.
    // false (with on_error_ fired) when the name cannot be resolved.
    bool resolve_target();

    // -- Helpers --------------------------------------------------------------
    bool create_wake_pipe();
    void close_wake_pipe();
    void wake() noexcept;
    void drain_wake_pipe() noexcept;

    // Sleeps up to `delay`; returns false if stop() was requested meanwhile.
    bool interruptible_sleep(std::chrono::milliseconds delay);
    // Waits for `fd` to become writable (or an error); false on timeout/stop.
    bool wait_writable(int fd);

    void adopt_connection(int fd);
    void close_connection();
    void fire_disconnected(std::string_view reason);

    static int make_nonblocking(int fd);
    static int tcp_nodelay(int fd);
    static int send_flags() noexcept;
    // TCP keepalive knob helper (see kKeepIdleSec); 0 on success.
    static int apply_keepalive(int fd);
    // Common setup for every connection socket: non-blocking, TCP_NODELAY,
    // SO_SNDBUF (cfg_.send_buffer_size), SIGPIPE suppression (SO_NOSIGPIPE
    // where it exists) and keepalive. 0 on success.
    int prepare_socket(int fd);

    // "" => keep looping, otherwise the connection must be torn down.
    std::string_view handle_recv(int fd);
    std::string_view handle_send(int fd);
};

} // namespace fix
