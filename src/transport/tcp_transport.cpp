// =============================================================================
// FIX Protocol Engine - TcpTransport implementation
// =============================================================================
#include "fix/transport/tcp_transport.hpp"
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <cassert>
#include <algorithm>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#endif

namespace fix {

TcpTransport::TcpTransport(TcpTransportConfig cfg)
    : cfg_(std::move(cfg))
{
    recv_buf_.resize(cfg_.recv_buffer_size);
}

TcpTransport::~TcpTransport() {
    stop();
}

Result<void> TcpTransport::start() {
    running_.store(true, std::memory_order_release);
    if (cfg_.initiator) {
        io_thread_ = std::thread([this]{ run_initiator(); });
    } else {
        io_thread_ = std::thread([this]{ run_acceptor(); });
    }
    return {};
}

void TcpTransport::stop() {
    running_.store(false, std::memory_order_release);
    close_connection();
#ifdef __linux__
    if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
#endif
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    if (io_thread_.joinable()) io_thread_.join();
}

int TcpTransport::make_nonblocking(int fd) {
#ifndef _WIN32
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    return 0;
#endif
}

int TcpTransport::tcp_nodelay(int fd) {
#ifndef _WIN32
    int one = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&one), sizeof(one));
#else
    return 0;
#endif
}

void TcpTransport::close_connection() {
    connected_.store(false, std::memory_order_release);
    if (conn_fd_ >= 0) {
#ifndef _WIN32
        ::shutdown(conn_fd_, SHUT_RDWR);
        ::close(conn_fd_);
#endif
        conn_fd_ = -1;
    }
}

void TcpTransport::do_connect(const std::string& host, std::uint16_t port) {
#ifndef _WIN32
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        if (on_error_) on_error_(make_error_code(ErrorCode::TransportError));
        return;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        if (on_error_) on_error_(make_error_code(ErrorCode::TransportError));
        return;
    }

    make_nonblocking(fd);
    tcp_nodelay(fd);

    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd);
        if (on_error_) on_error_(make_error_code(ErrorCode::TransportError));
        return;
    }

    conn_fd_ = fd;
    connected_.store(true, std::memory_order_release);
    if (on_connected_) on_connected_();
    run_event_loop(fd);
#endif
}

void TcpTransport::run_initiator() {
    while (running_.load(std::memory_order_acquire)) {
        do_connect(cfg_.host, cfg_.port);
        if (!running_.load(std::memory_order_acquire)) break;
        // Reconnect delay
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void TcpTransport::run_acceptor() {
#ifndef _WIN32
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        if (on_error_) on_error_(make_error_code(ErrorCode::TransportError));
        return;
    }

    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    make_nonblocking(listen_fd_);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(cfg_.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_); listen_fd_ = -1;
        if (on_error_) on_error_(make_error_code(ErrorCode::TransportError));
        return;
    }
    ::listen(listen_fd_, cfg_.backlog);

    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in peer{};
        socklen_t   len = sizeof(peer);
        int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            break;
        }
        make_nonblocking(fd);
        tcp_nodelay(fd);
        conn_fd_ = fd;
        connected_.store(true, std::memory_order_release);
        if (on_connected_) on_connected_();
        run_event_loop(fd);
    }
#endif
}

void TcpTransport::run_event_loop(int fd) {
#ifdef __linux__
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) return;

    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLHUP | EPOLLERR;
    ev.data.fd = fd;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);

    epoll_event events[64];
    while (running_.load(std::memory_order_acquire)) {
        int n = ::epoll_wait(epoll_fd_, events, 64, 100 /*ms*/);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                close_connection();
                if (on_disconnected_) on_disconnected_("connection closed");
                ::close(epoll_fd_); epoll_fd_ = -1;
                return;
            }
            if (events[i].events & EPOLLIN)  handle_recv(fd);
            if (events[i].events & EPOLLOUT) handle_send(fd);
        }
    }
    ::close(epoll_fd_); epoll_fd_ = -1;
#else
    // Fallback: simple blocking read loop (non-Linux)
    while (running_.load(std::memory_order_acquire)) {
        handle_recv(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
    close_connection();
    if (on_disconnected_) on_disconnected_("transport stopped");
}

void TcpTransport::handle_recv(int fd) {
#ifndef _WIN32
    while (true) {
        ssize_t n = ::recv(fd, recv_buf_.data(), recv_buf_.size(), 0);
        if (n > 0) {
            if (on_data_) on_data_(recv_buf_.data(), static_cast<std::size_t>(n));
        } else if (n == 0) {
            close_connection();
            if (on_disconnected_) on_disconnected_("peer closed");
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            close_connection();
            if (on_error_) on_error_(std::error_code(errno, std::system_category()));
            return;
        }
    }
#endif
}

void TcpTransport::handle_send(int fd) {
#ifndef _WIN32
    std::lock_guard lock(send_mutex_);
    while (!send_queue_.empty()) {
        auto& front = send_queue_.front();
        ssize_t n = ::send(fd, front.data(), front.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            break;
        }
        if (static_cast<std::size_t>(n) < front.size()) {
            front.erase(0, static_cast<std::size_t>(n));
            return;
        }
        send_queue_.pop_front();
    }
#endif
}

Result<void> TcpTransport::send(const char* data, std::size_t len) {
    if (!connected_.load(std::memory_order_acquire))
        return make_unexpected(ErrorCode::TransportError);
#ifndef _WIN32
    std::lock_guard lock(send_mutex_);
    if (send_queue_.empty()) {
        // Try direct send first
        ssize_t n = ::send(conn_fd_, data, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return make_unexpected(ErrorCode::TransportError);
            n = 0;
        }
        if (static_cast<std::size_t>(n) < len) {
            send_queue_.emplace_back(data + n, len - static_cast<std::size_t>(n));
        }
    } else {
        send_queue_.emplace_back(data, len);
    }
#endif
    return {};
}

std::unique_ptr<ITransport> make_tcp_transport(TcpTransportConfig cfg) {
    return std::make_unique<TcpTransport>(std::move(cfg));
}

} // namespace fix
