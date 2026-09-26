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
    explicit TcpTransport(TcpTransportConfig cfg);
    ~TcpTransport() override;

    void set_on_connected(ConnectedCallback cb) override { on_connected_ = std::move(cb); }
    void set_on_disconnected(DisconnectedCallback cb) override { on_disconnected_ = std::move(cb); }
    void set_on_data(DataCallback cb) override { on_data_ = std::move(cb); }
    void set_on_error(ErrorCallback cb) override { on_error_ = std::move(cb); }

    Result<void> start() override;
    void stop() override;
    void disconnect() override;
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

    // Self-pipe used to interrupt epoll_wait()/poll() from stop() and to make
    // the reconnect backoff sleep interruptible. Both ends stay open until the
    // IO thread has been joined, so writing to it can never raise SIGPIPE.
    int wake_pipe_[2] = {-1, -1};

    // Send queue
    std::mutex send_mutex_;
    std::deque<std::string> send_queue_;
    std::vector<char> recv_buf_;

    // -- Threads --------------------------------------------------------------
    void run_acceptor();
    void run_initiator();

    // Serve one connection until it dies. Returns the disconnect reason
    // (empty string => the transport was stopped).
    std::string_view run_event_loop(int fd);

    // Non-blocking connect incl. wait-for-writable + SO_ERROR verification.
    // Returns true once a connection has been established (the event loop runs
    // to completion before returning).
    bool do_connect(const std::string &host, std::uint16_t port);

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

    // "" => keep looping, otherwise the connection must be torn down.
    std::string_view handle_recv(int fd);
    std::string_view handle_send(int fd);
};

} // namespace fix
