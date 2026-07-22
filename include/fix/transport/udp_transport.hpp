#pragma once
// =============================================================================
// FIX Protocol Engine - UDP Transport
//
// Supports:
//   * Unicast UDP (send/receive between two endpoints)
//   * IP Multicast (join group, send to group – useful for market-data feeds)
//
// Notes:
//   - UDP is connectionless; `is_connected()` returns true once the socket is
//     successfully bound/opened.
//   - Because UDP is unreliable, this transport is most appropriate for
//     one-way market-data broadcasts.  For order-management (where FIX
//     sequence integrity is required) use TcpTransport instead.
//   - on_connected is fired once the socket is ready.
//   - on_disconnected is fired only when stop() is called.
// =============================================================================
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "transport.hpp"

#ifdef __linux__
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fix {

class UdpTransport : public ITransport {
public:
    explicit UdpTransport(UdpTransportConfig cfg);
    ~UdpTransport() override;

    void set_on_connected(ConnectedCallback cb) override { on_connected_ = std::move(cb); }
    void set_on_disconnected(DisconnectedCallback cb) override { on_disconnected_ = std::move(cb); }
    void set_on_data(DataCallback cb) override { on_data_ = std::move(cb); }
    void set_on_error(ErrorCallback cb) override { on_error_ = std::move(cb); }

    Result<void> start() override;
    void stop() override;
    Result<void> send(const char *data, std::size_t len) override;

    [[nodiscard]] bool is_connected() const noexcept override {
        return socket_ready_.load(std::memory_order_acquire);
    }
    [[nodiscard]] TransportProtocol protocol() const noexcept override {
        return TransportProtocol::UDP;
    }

private:
    UdpTransportConfig cfg_;

    ConnectedCallback on_connected_;
    DisconnectedCallback on_disconnected_;
    DataCallback on_data_;
    ErrorCallback on_error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> socket_ready_{false};
    std::thread recv_thread_;

    int sock_fd_ = -1;

    // Resolved remote address (for sending)
    struct sockaddr_in remote_addr_ {};
    bool remote_resolved_ = false;

    Result<void> open_socket();
    void close_socket();
    void recv_loop();
};

} // namespace fix
