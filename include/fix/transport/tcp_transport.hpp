#pragma once
// =============================================================================
// FIX Protocol Engine - TCP Transport (epoll-based, Linux/POSIX)
// =============================================================================
#include <atomic>
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
    Result<void> send(const char *data, std::size_t len) override;
    [[nodiscard]] bool is_connected() const noexcept override {
        return connected_.load(std::memory_order_acquire);
    }

private:
    TcpTransportConfig cfg_;
    ConnectedCallback on_connected_;
    DisconnectedCallback on_disconnected_;
    DataCallback on_data_;
    ErrorCallback on_error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread io_thread_;

    // Socket handles
    int listen_fd_ = -1;
    int conn_fd_ = -1;
#ifdef __linux__
    int epoll_fd_ = -1;
#endif

    // Send queue
    std::mutex send_mutex_;
    std::deque<std::string> send_queue_;
    std::vector<char> recv_buf_;

    void run_acceptor();
    void run_initiator();
    void run_event_loop(int fd);
    void do_connect(const std::string &host, std::uint16_t port);

    static int make_nonblocking(int fd);
    static int tcp_nodelay(int fd);
    void handle_recv(int fd);
    void handle_send(int fd);
    void close_connection();
};

} // namespace fix
