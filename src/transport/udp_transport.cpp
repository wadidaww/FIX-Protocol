// =============================================================================
// FIX Protocol Engine - UdpTransport implementation
// =============================================================================
#include "fix/transport/udp_transport.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fix {

UdpTransport::UdpTransport(UdpTransportConfig cfg)
    : cfg_(std::move(cfg)) {}

UdpTransport::~UdpTransport() {
    stop();
}

// ---------------------------------------------------------------------------
Result<void> UdpTransport::start() {
    auto r = open_socket();
    if (!r)
        return r;

    running_.store(true, std::memory_order_release);
    // Only start the recv loop for acceptors (receivers) or multicast members.
    // Initiators that only send still need the socket open, but we spawn the
    // thread for them too so they can receive replies if needed.
    recv_thread_ = std::thread([this] { recv_loop(); });
    return {};
}

void UdpTransport::stop() {
    running_.store(false, std::memory_order_release);
    close_socket();
    if (recv_thread_.joinable())
        recv_thread_.join();
}

// ---------------------------------------------------------------------------
Result<void> UdpTransport::open_socket() {
#ifndef _WIN32
    sock_fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_fd_ < 0) {
        if (on_error_)
            on_error_(make_error_code(ErrorCode::TransportError));
        return make_unexpected(ErrorCode::TransportError);
    }

    // Allow reuse of the local address so multiple processes / restarts work.
    int one = 1;
    ::setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one),
                 sizeof(one));
#ifdef SO_REUSEPORT
    ::setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&one),
                 sizeof(one));
#endif

    // Bind the socket so we can receive.
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(cfg_.bind_port);

    if (!cfg_.bind_host.empty()) {
        if (::inet_pton(AF_INET, cfg_.bind_host.c_str(), &bind_addr.sin_addr) <= 0) {
            ::close(sock_fd_);
            sock_fd_ = -1;
            return make_unexpected(ErrorCode::TransportError);
        }
    } else {
        bind_addr.sin_addr.s_addr = INADDR_ANY;
    }

    // For multicast, bind to the group address so that the kernel demultiplexes
    // datagrams correctly.
    if (cfg_.multicast && !cfg_.multicast_group.empty()) {
        ::inet_pton(AF_INET, cfg_.multicast_group.c_str(), &bind_addr.sin_addr);
    }

    if (::bind(sock_fd_, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
        if (on_error_)
            on_error_(make_error_code(ErrorCode::TransportError));
        return make_unexpected(ErrorCode::TransportError);
    }

    // Join multicast group if requested.
    if (cfg_.multicast && !cfg_.multicast_group.empty()) {
        ip_mreq mreq{};
        ::inet_pton(AF_INET, cfg_.multicast_group.c_str(), &mreq.imr_multiaddr);
        if (!cfg_.multicast_iface.empty()) {
            ::inet_pton(AF_INET, cfg_.multicast_iface.c_str(), &mreq.imr_interface);
        } else {
            mreq.imr_interface.s_addr = INADDR_ANY;
        }
        if (::setsockopt(sock_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         reinterpret_cast<const char *>(&mreq), sizeof(mreq)) < 0) {
            ::close(sock_fd_);
            sock_fd_ = -1;
            if (on_error_)
                on_error_(make_error_code(ErrorCode::TransportError));
            return make_unexpected(ErrorCode::TransportError);
        }

        // Set TTL for outgoing multicast datagrams.
        unsigned char ttl = cfg_.multicast_ttl;
        ::setsockopt(sock_fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                     reinterpret_cast<const char *>(&ttl), sizeof(ttl));

        // Set outgoing multicast interface if provided.
        if (!cfg_.multicast_iface.empty()) {
            struct in_addr iface{};
            ::inet_pton(AF_INET, cfg_.multicast_iface.c_str(), &iface);
            ::setsockopt(sock_fd_, IPPROTO_IP, IP_MULTICAST_IF,
                         reinterpret_cast<const char *>(&iface), sizeof(iface));
        }
    }

    // Resolve remote address for initiators.
    if (cfg_.initiator && !cfg_.remote_host.empty() && cfg_.remote_port != 0) {
        std::memset(&remote_addr_, 0, sizeof(remote_addr_));
        remote_addr_.sin_family = AF_INET;
        remote_addr_.sin_port = htons(cfg_.remote_port);

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        std::string port_str = std::to_string(cfg_.remote_port);
        if (::getaddrinfo(cfg_.remote_host.c_str(), port_str.c_str(), &hints, &res) == 0 &&
            res != nullptr) {
            remote_addr_ = *reinterpret_cast<sockaddr_in *>(res->ai_addr);
            ::freeaddrinfo(res);
            remote_resolved_ = true;
        } else {
            // Fall back to inet_pton for numeric IPs.
            if (::inet_pton(AF_INET, cfg_.remote_host.c_str(), &remote_addr_.sin_addr) > 0) {
                remote_resolved_ = true;
            }
        }
    } else if (cfg_.multicast && !cfg_.multicast_group.empty() && cfg_.remote_port != 0) {
        // Multicast send target.
        std::memset(&remote_addr_, 0, sizeof(remote_addr_));
        remote_addr_.sin_family = AF_INET;
        remote_addr_.sin_port = htons(cfg_.remote_port);
        ::inet_pton(AF_INET, cfg_.multicast_group.c_str(), &remote_addr_.sin_addr);
        remote_resolved_ = true;
    }

    // Set non-blocking.
    int flags = ::fcntl(sock_fd_, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(sock_fd_, F_SETFL, flags | O_NONBLOCK);

    socket_ready_.store(true, std::memory_order_release);
    if (on_connected_)
        on_connected_();

    return {};
#else
    return make_unexpected(ErrorCode::TransportError);
#endif
}

void UdpTransport::close_socket() {
    socket_ready_.store(false, std::memory_order_release);
#ifndef _WIN32
    if (sock_fd_ >= 0) {
        // Leave multicast group before closing.
        if (cfg_.multicast && !cfg_.multicast_group.empty()) {
            ip_mreq mreq{};
            ::inet_pton(AF_INET, cfg_.multicast_group.c_str(), &mreq.imr_multiaddr);
            mreq.imr_interface.s_addr = INADDR_ANY;
            ::setsockopt(sock_fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                         reinterpret_cast<const char *>(&mreq), sizeof(mreq));
        }
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
#endif
}

// ---------------------------------------------------------------------------
void UdpTransport::recv_loop() {
#ifndef _WIN32
    std::vector<char> buf(cfg_.recv_buffer_size);
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);

    while (running_.load(std::memory_order_acquire)) {
        ssize_t n = ::recvfrom(sock_fd_, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr *>(&peer), &peer_len);
        if (n > 0) {
            if (on_data_)
                on_data_(buf.data(), static_cast<std::size_t>(n));
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // Non-blocking: no data yet; yield and retry.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (running_.load(std::memory_order_acquire) && on_error_)
                on_error_(std::error_code(errno, std::system_category()));
            break;
        }
    }

    if (on_disconnected_)
        on_disconnected_("udp transport stopped");
#endif
}

// ---------------------------------------------------------------------------
Result<void> UdpTransport::send(const char *data, std::size_t len) {
    if (!socket_ready_.load(std::memory_order_acquire))
        return make_unexpected(ErrorCode::TransportError);

#ifndef _WIN32
    if (!remote_resolved_)
        return make_unexpected(ErrorCode::TransportError);

    ssize_t n = ::sendto(sock_fd_, data, len, 0,
                         reinterpret_cast<const sockaddr *>(&remote_addr_), sizeof(remote_addr_));
    if (n < 0)
        return make_unexpected(ErrorCode::TransportError);
#endif
    return {};
}

std::unique_ptr<ITransport> make_udp_transport(UdpTransportConfig cfg) {
    return std::make_unique<UdpTransport>(std::move(cfg));
}

} // namespace fix
