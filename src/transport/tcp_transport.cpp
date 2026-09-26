// =============================================================================
// FIX Protocol Engine - TcpTransport implementation
// =============================================================================
#include "fix/transport/tcp_transport.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#endif

namespace fix {

namespace {

// Every blocking wait in the IO thread wakes at least this often so that
// `running_` is re-checked even if the wake pipe is unavailable.
constexpr int kPollTimeoutMs = 100;
// Acceptor backoff after a transient accept() failure (EMFILE, ECONNABORTED...).
constexpr std::chrono::milliseconds kAcceptBackoff{100};
// A black-holed SYN must not pin the IO thread forever; stop() interrupts this
// earlier through the wake pipe.
constexpr std::chrono::milliseconds kConnectTimeout{30000};

} // namespace

TcpTransport::TcpTransport(TcpTransportConfig cfg)
    : cfg_(std::move(cfg)) {
    recv_buf_.resize(cfg_.recv_buffer_size);
}

TcpTransport::~TcpTransport() {
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<void> TcpTransport::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return {}; // already started – idempotent

    if (!create_wake_pipe()) {
        running_.store(false, std::memory_order_release);
        if (on_error_)
            on_error_(make_error_code(ErrorCode::TransportError));
        return make_unexpected(ErrorCode::TransportError);
    }

    // A previous stop() from a transport callback leaves the thread unjoined;
    // std::thread assignment would otherwise terminate.
    if (io_thread_.joinable()) {
        if (io_thread_.get_id() == std::this_thread::get_id()) {
            running_.store(false, std::memory_order_release);
            return make_unexpected(ErrorCode::TransportError);
        }
        io_thread_.join();
    }

    io_thread_ = std::thread([this] {
        // Publish "this thread is a transport IO thread (of this transport)"
        // for on_io_thread()/on_transport_io_thread(); the thread_local
        // storage expires with the thread, so nothing needs clearing.
        detail::current_transport_io = this;
        if (cfg_.initiator)
            run_initiator();
        else
            run_acceptor();
    });
    return {};
}

void TcpTransport::stop() {
    running_.store(false, std::memory_order_release);

    if (!io_thread_.joinable()) {
        // Never started (or already stopped).
        close_wake_pipe();
        return;
    }
    if (io_thread_.get_id() == std::this_thread::get_id()) {
        // stop() was invoked from a transport callback running on the IO
        // thread: joining would deadlock. The thread observes running_ == false
        // and exits; a later stop()/destructor performs the join – or the
        // Engine parks the transport in its reap list so the destructor never
        // runs on this thread.
        return;
    }
    wake(); // interrupt epoll_wait()/poll()/backoff sleep
    io_thread_.join();
    close_wake_pipe(); // only now – nobody can be writing to the pipe anymore
}

void TcpTransport::disconnect() {
    {
        std::lock_guard lock(send_mutex_);
        if (conn_fd_ < 0)
            return; // no live connection – nothing to close (no-op)
        // Serialized with close_connection()/adopt_connection() (same mutex),
        // so the flag can only be raised while a connection is actually live,
        // and both teardown and the next adopt clear it: a stale request can
        // never leak into the next session.
        disconnect_requested_.store(true, std::memory_order_release);
    }
    wake(); // make the IO thread re-check promptly
}

// ---------------------------------------------------------------------------
// Wake pipe (self-pipe trick)
// ---------------------------------------------------------------------------
bool TcpTransport::create_wake_pipe() {
#ifdef _WIN32
    return true; // transport bodies are no-ops on Windows – nothing to wake
#else
    if (wake_pipe_[0] >= 0)
        return true;
    if (::pipe(wake_pipe_) != 0) {
        wake_pipe_[0] = wake_pipe_[1] = -1;
        return false;
    }
    for (int &fd : wake_pipe_) {
        if (make_nonblocking(fd) != 0) {
            close_wake_pipe();
            return false;
        }
        (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return true;
#endif
}

void TcpTransport::close_wake_pipe() {
#ifndef _WIN32
    for (int &fd : wake_pipe_) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
#endif
}

void TcpTransport::wake() noexcept {
#ifndef _WIN32
    if (wake_pipe_[1] < 0)
        return;
    char b = 'w';
    ssize_t rc;
    do {
        rc = ::write(wake_pipe_[1], &b, 1);
    } while (rc < 0 && errno == EINTR);
    // EPIPE cannot occur: the read end stays open until after the join.
    (void)rc; // EAGAIN (pipe full) – the loops also have bounded poll timeouts
#endif
}

void TcpTransport::drain_wake_pipe() noexcept {
#ifndef _WIN32
    if (wake_pipe_[0] < 0)
        return;
    char buf[64];
    while (true) {
        ssize_t n = ::read(wake_pipe_[0], buf, sizeof(buf));
        if (n > 0)
            continue;
        if (n < 0 && errno == EINTR)
            continue;
        break; // EAGAIN/EWOULDBLOCK: drained
    }
#endif
}

bool TcpTransport::interruptible_sleep(std::chrono::milliseconds delay) {
#ifndef _WIN32
    if (delay.count() <= 0)
        return running_.load(std::memory_order_acquire);
    if (wake_pipe_[0] < 0) {
        std::this_thread::sleep_for(delay);
        return running_.load(std::memory_order_acquire);
    }
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (running_.load(std::memory_order_acquire)) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
            break;
        pollfd pfd{};
        pfd.fd = wake_pipe_[0];
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, static_cast<int>(remaining.count()) + 1);
        if (rc > 0) {
            drain_wake_pipe();
            // Only a stop request ends the sleep early. A wake that raced a
            // disconnect()/spurious write just re-checks running_ and keeps
            // sleeping, so a stale byte can never kill the reconnect loop.
            if (!running_.load(std::memory_order_acquire))
                return false;
            continue;
        }
        if (rc < 0 && errno != EINTR) {
            // A poll() error is *not* a stop request: treating it as one made
            // the transport silently exit its accept/reconnect loops while
            // running_ was still true. Fall back to a short sleep and keep
            // looping; only !running_ (or a wake-pipe stop) ends the sleep.
            if (on_error_)
                on_error_(std::error_code(errno, std::system_category()));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // rc == 0 (slice expired) and EINTR just re-check the deadline.
    }
    return running_.load(std::memory_order_acquire);
#else
    // No wake pipe on Windows – sleep in slices so stop() stays responsive.
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(50)));
    }
    return running_.load(std::memory_order_acquire);
#endif
}

// ---------------------------------------------------------------------------
// Connecting (initiator)
// ---------------------------------------------------------------------------
bool TcpTransport::wait_writable(int fd) {
#ifdef _WIN32
    (void)fd;
    return false;
#else
    const auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
    while (running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false; // connect timeout
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int timeout_ms = static_cast<int>(remaining.count()) + 1;

        pollfd pfds[2]{};
        int nfds = 1;
        pfds[0].fd = fd;
        pfds[0].events = POLLOUT;
        if (wake_pipe_[0] >= 0) {
            pfds[1].fd = wake_pipe_[0];
            pfds[1].events = POLLIN;
            nfds = 2;
        }

        const int rc = ::poll(pfds, static_cast<nfds_t>(nfds), timeout_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            // Non-EINTR error: not a stop request either – abandon this
            // address and let the caller move on / retry.
            return false;
        }
        if (rc == 0)
            continue; // slice expired – re-check the deadline
        if (nfds == 2 && pfds[1].revents != 0) {
            drain_wake_pipe();
            if (!running_.load(std::memory_order_acquire))
                return false; // stop()
            continue;         // spurious wake (stale byte) – keep waiting
        }
        if (pfds[0].revents != 0)
            return true; // writable *or* error – SO_ERROR disambiguates
    }
    return false;
#endif
}

bool TcpTransport::do_connect(const std::string &host, std::uint16_t port) {
#ifdef _WIN32
    (void)host;
    (void)port;
    return false;
#else
    struct addrinfo hints {
    }, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        if (on_error_)
            on_error_(make_error_code(ErrorCode::TransportError));
        return false;
    }

    bool established = false;
    for (struct addrinfo *ai = res;
         ai != nullptr && !established && running_.load(std::memory_order_acquire);
         ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (make_nonblocking(fd) != 0 || tcp_nodelay(fd) != 0) {
            ::close(fd);
            continue;
        }
#ifdef SO_NOSIGPIPE
        int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            ::close(fd);
            continue;
        }
        if (rc != 0 && !wait_writable(fd)) {
            ::close(fd);
            if (!running_.load(std::memory_order_acquire))
                break; // stop() during connect
            continue;
        }

        // Verify the outcome – a failed connect still reports a writable fd.
        int err = 0;
        socklen_t err_len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0 || err != 0) {
            ::close(fd);
            continue;
        }

        adopt_connection(fd);
        if (on_connected_)
            on_connected_(); // session may Logon immediately – fd is ready
        const auto reason = run_event_loop(fd);
        close_connection();
        fire_disconnected(reason);
        established = true;
    }
    ::freeaddrinfo(res);

    if (!established && running_.load(std::memory_order_acquire) && on_error_)
        on_error_(make_error_code(ErrorCode::TransportError));
    return established;
#endif
}

void TcpTransport::run_initiator() {
#ifndef _WIN32
    using ms = std::chrono::milliseconds;
    // Exponential backoff: starts at reconnect_delay, doubles per failed
    // attempt up to reconnect_delay_max, resets after an established connection.
    auto delay = std::max(cfg_.reconnect_delay, ms{0});
    const auto delay_max = std::max({delay, cfg_.reconnect_delay_max, ms{1}});

    while (running_.load(std::memory_order_acquire)) {
        const bool established = do_connect(cfg_.host, cfg_.port);
        if (!running_.load(std::memory_order_acquire))
            break;
        if (!interruptible_sleep(delay))
            break; // stop() during backoff
        delay = std::clamp(established ? cfg_.reconnect_delay : delay * 2, ms{1}, delay_max);
    }
#endif
}

// ---------------------------------------------------------------------------
// Accepting (acceptor)
// ---------------------------------------------------------------------------
void TcpTransport::run_acceptor() {
#ifndef _WIN32
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        if (on_error_)
            on_error_(make_error_code(ErrorCode::TransportError));
        return;
    }

    int one = 1;
    (void)::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    (void)make_nonblocking(lfd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(lfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
        ::listen(lfd, cfg_.backlog) < 0) {
        ::close(lfd);
        if (on_error_)
            on_error_(make_error_code(ErrorCode::TransportError));
        return;
    }
    listen_fd_ = lfd; // owned by this thread; closed before it returns

    while (running_.load(std::memory_order_acquire)) {
        // Block until a client connects or stop() writes the wake pipe –
        // no more 10 ms accept() busy-loop.
        pollfd pfds[2]{};
        int nfds = 1;
        pfds[0].fd = listen_fd_;
        pfds[0].events = POLLIN;
        if (wake_pipe_[0] >= 0) {
            pfds[1].fd = wake_pipe_[0];
            pfds[1].events = POLLIN;
            nfds = 2;
        }

        const int rc = ::poll(pfds, static_cast<nfds_t>(nfds), kPollTimeoutMs);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            if (on_error_)
                on_error_(std::error_code(errno, std::system_category()));
            break;
        }
        if (nfds == 2 && pfds[1].revents != 0) {
            drain_wake_pipe();
            if (!running_.load(std::memory_order_acquire))
                break;
        }
        if ((pfds[0].revents & POLLIN) == 0) {
            if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                if (on_error_)
                    on_error_(make_error_code(ErrorCode::TransportError));
                break;
            }
            continue;
        }

        // Drain every connection queued in the backlog. After a connection
        // drops we always come back here – the acceptor serves more than one
        // client over its lifetime.
        while (running_.load(std::memory_order_acquire)) {
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&peer), &len);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break; // backlog drained
                // Transient failure (EMFILE/ENOBUFS/ECONNABORTED): report, back
                // off, then let the outer poll() re-evaluate the socket.
                if (on_error_)
                    on_error_(std::error_code(errno, std::system_category()));
                (void)interruptible_sleep(kAcceptBackoff);
                break;
            }
            (void)make_nonblocking(fd);
            (void)tcp_nodelay(fd);
#ifdef SO_NOSIGPIPE
            (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

            adopt_connection(fd);
            if (on_connected_)
                on_connected_();
            const auto reason = run_event_loop(fd);
            close_connection();
            fire_disconnected(reason);
            break; // serve one connection at a time, then re-enter accept()
        }
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
#endif
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------
std::string_view TcpTransport::run_event_loop(int fd) {
#ifdef __linux__
    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        if (on_error_)
            on_error_(make_error_code(ErrorCode::TransportError));
        return "epoll_create1 failed";
    }

    auto add_fd = [epoll_fd](int target, std::uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = target;
        return ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, target, &ev) == 0;
    };

    // EPOLLRDHUP so a peer half-close is reported even without payload; the
    // wake pipe makes stop() interrupt epoll_wait() immediately.
    const bool registered =
        add_fd(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR) &&
        (wake_pipe_[0] < 0 || add_fd(wake_pipe_[0], EPOLLIN | EPOLLET));
    if (!registered) {
        ::close(epoll_fd);
        if (on_error_)
            on_error_(make_error_code(ErrorCode::TransportError));
        return "epoll_ctl failed";
    }

    std::string_view reason;
    bool done = false;
    epoll_event events[64];
    while (!done && running_.load(std::memory_order_acquire)) {
        if (disconnect_requested_.load(std::memory_order_acquire)) {
            // Session-initiated disconnect (CompID mismatch, heartbeat
            // timeout, …): tear this connection down; the caller clears the
            // flag in close_connection() and continues its outer loop.
            reason = "disconnect requested";
            break;
        }
        const int n = ::epoll_wait(epoll_fd, events, 64, kPollTimeoutMs);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            reason = "epoll_wait failed";
            break;
        }

        for (int i = 0; i < n && !done; ++i) {
            const int efd = events[i].data.fd;
            const std::uint32_t evs = events[i].events;

            if (wake_pipe_[0] >= 0 && efd == wake_pipe_[0]) {
                drain_wake_pipe();
                if (!running_.load(std::memory_order_acquire))
                    done = true; // stop(): `reason` stays empty
                continue;
            }
            if (evs & (EPOLLHUP | EPOLLERR)) {
                reason = "connection error";
                done = true;
                break;
            }
            if (evs & (EPOLLIN | EPOLLRDHUP)) {
                reason = handle_recv(fd); // "" = drained, otherwise teardown
                if (!reason.empty()) {
                    done = true;
                    break;
                }
            }
            if (evs & EPOLLOUT) {
                reason = handle_send(fd);
                if (!reason.empty()) {
                    done = true;
                    break;
                }
            }
        }

        // Always try to flush: bytes queued by another thread while the socket
        // was already writable produce no further edge-triggered EPOLLOUT.
        if (!done) {
            reason = handle_send(fd);
            if (!reason.empty())
                done = true;
        }
    }

    ::close(epoll_fd); // IO thread closes its own epoll fd – never stop()
    return reason;
#elif !defined(_WIN32)
    // poll() fallback (macOS/BSD): level-triggered, so nothing is missed.
    std::string_view reason;
    while (running_.load(std::memory_order_acquire)) {
        if (disconnect_requested_.load(std::memory_order_acquire)) {
            reason = "disconnect requested"; // cleared by close_connection()
            break;
        }
        pollfd pfds[2]{};
        int nfds = 1;
        pfds[0].fd = fd;
        pfds[0].events = POLLIN;
        {
            // Only arm POLLOUT while there is something to flush.
            std::lock_guard lock(send_mutex_);
            if (!send_queue_.empty())
                pfds[0].events |= POLLOUT;
        }
        if (wake_pipe_[0] >= 0) {
            pfds[1].fd = wake_pipe_[0];
            pfds[1].events = POLLIN;
            nfds = 2;
        }

        const int rc = ::poll(pfds, static_cast<nfds_t>(nfds), kPollTimeoutMs);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            reason = "poll failed";
            break;
        }
        if (rc == 0)
            continue;
        if (nfds == 2 && pfds[1].revents != 0) {
            drain_wake_pipe();
            if (!running_.load(std::memory_order_acquire))
                break; // stop(): reason stays empty
            continue;
        }
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            reason = "connection error";
            break;
        }
        if (pfds[0].revents & POLLIN) {
            reason = handle_recv(fd);
            if (!reason.empty())
                break;
        }
        if (pfds[0].revents & POLLOUT) {
            reason = handle_send(fd);
            if (!reason.empty())
                break;
        }

        reason = handle_send(fd); // flush anything queued concurrently
        if (!reason.empty())
            break;
    }
    return reason;
#else
    (void)fd;
    return "unsupported platform";
#endif
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------
std::string_view TcpTransport::handle_recv(int fd) {
#ifdef _WIN32
    (void)fd;
    return {};
#else
    while (true) {
        const ssize_t n = ::recv(fd, recv_buf_.data(), recv_buf_.size(), 0);
        if (n > 0) {
            if (on_data_)
                on_data_(recv_buf_.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
            return "peer closed"; // FIN: return so the caller tears down
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return {}; // drained
        if (errno == EINTR)
            continue;
        if (on_error_)
            on_error_(std::error_code(errno, std::system_category()));
        return "recv failed";
    }
#endif
}

std::string_view TcpTransport::handle_send(int fd) {
#ifdef _WIN32
    (void)fd;
    return {};
#else
    int err = 0;
    {
        std::lock_guard lock(send_mutex_);
        while (!send_queue_.empty()) {
            const std::string &front = send_queue_.front();
            const ssize_t n = ::send(fd, front.data(), front.size(), send_flags());
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return {}; // kernel buffer full – retry on next edge/tick
                if (errno == EINTR)
                    continue;
                err = errno;
                break;
            }
            if (static_cast<std::size_t>(n) < front.size()) {
                send_queue_.front().erase(0, static_cast<std::size_t>(n));
                return {};
            }
            send_queue_.pop_front();
        }
        if (err == 0)
            return {};
    }
    // Fatal send error: report outside the lock, then let the caller tear the
    // connection down (clearing the queue).
    if (on_error_)
        on_error_(std::error_code(err, std::system_category()));
    return "send failed";
#endif
}

Result<void> TcpTransport::send(const char *data, std::size_t len) {
    if (!connected_.load(std::memory_order_acquire))
        return make_unexpected(ErrorCode::TransportError);
#ifndef _WIN32
    std::lock_guard lock(send_mutex_);
    if (conn_fd_ < 0)
        return make_unexpected(ErrorCode::TransportError);
    if (send_queue_.empty()) {
        // Try a direct write first; queue whatever did not fit so the event
        // loop flushes it (the socket is non-writable at this point, which is
        // exactly what generates the next edge-triggered EPOLLOUT).
        ssize_t n;
        do {
            n = ::send(conn_fd_, data, len, send_flags());
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return make_unexpected(ErrorCode::TransportError);
            n = 0;
        }
        if (static_cast<std::size_t>(n) < len)
            send_queue_.emplace_back(data + n, len - static_cast<std::size_t>(n));
    } else {
        send_queue_.emplace_back(data, len);
    }
#else
    (void)data;
    (void)len;
#endif
    return {};
}

// ---------------------------------------------------------------------------
// Connection ownership (IO thread only)
// ---------------------------------------------------------------------------
void TcpTransport::adopt_connection(int fd) {
    std::lock_guard lock(send_mutex_);
    conn_fd_ = fd;
    connected_.store(true, std::memory_order_release);
    send_queue_.clear(); // never carry bytes across connections
    // A disconnect() aimed at the *previous* connection must not kill this
    // one: the flag is only ever raised while conn_fd_ >= 0 (same mutex), so
    // clearing it here leaves no path for a stale request to leak through.
    disconnect_requested_.store(false, std::memory_order_release);
}

void TcpTransport::close_connection() {
    int fd = -1;
    {
        std::lock_guard lock(send_mutex_);
        fd = conn_fd_;
        conn_fd_ = -1;
        send_queue_.clear(); // stale bytes must not hit the next connection
        // Teardown consumes any pending disconnect request. Both orderings are
        // safe under this mutex: if disconnect() ran first the flag is set
        // here and cleared now; if close_connection() ran first conn_fd_ is
        // already -1, so a later disconnect() is a no-op that cannot raise the
        // flag for the next connection (which clears it again on adopt).
        disconnect_requested_.store(false, std::memory_order_release);
    }
    connected_.store(false, std::memory_order_release);
    if (fd >= 0) {
#ifndef _WIN32
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd); // the IO thread closes fds it opened – no stop() race
#endif
    }
}

void TcpTransport::fire_disconnected(std::string_view reason) {
    if (reason.empty())
        reason = "transport stopped";
    if (on_disconnected_)
        on_disconnected_(reason);
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------
int TcpTransport::make_nonblocking(int fd) {
#ifndef _WIN32
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    (void)fd;
    return 0;
#endif
}

int TcpTransport::tcp_nodelay(int fd) {
#ifndef _WIN32
    int one = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one),
                        sizeof(one));
#else
    (void)fd;
    return 0;
#endif
}

int TcpTransport::send_flags() noexcept {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL; // never raise SIGPIPE on a half-open peer
#else
    return 0; // macOS/BSD: SO_NOSIGPIPE is set on the socket instead
#endif
}

std::unique_ptr<ITransport> make_tcp_transport(TcpTransportConfig cfg) {
    return std::make_unique<TcpTransport>(std::move(cfg));
}

} // namespace fix
