#pragma once
// =============================================================================
// FIX Protocol Engine - Transport abstraction
// =============================================================================
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../core/types.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// Transport protocol selector
// ---------------------------------------------------------------------------
enum class TransportProtocol : std::uint8_t {
    TCP = 0, // Reliable, ordered stream – required for FIX session layer
    UDP = 1, // Unreliable datagrams – market data / low-latency feeds
};

// ---------------------------------------------------------------------------
// Transport events
// ---------------------------------------------------------------------------
using ConnectedCallback = std::function<void()>;
using DisconnectedCallback = std::function<void(std::string_view reason)>;
using DataCallback = std::function<void(const char *data, std::size_t len)>;
using ErrorCallback = std::function<void(std::error_code ec)>;

// ---------------------------------------------------------------------------
// ITransport - abstract network transport
// ---------------------------------------------------------------------------
class ITransport {
public:
    virtual ~ITransport() = default;

    // Set event callbacks
    virtual void set_on_connected(ConnectedCallback cb) = 0;
    virtual void set_on_disconnected(DisconnectedCallback cb) = 0;
    virtual void set_on_data(DataCallback cb) = 0;
    virtual void set_on_error(ErrorCallback cb) = 0;

    // Start connecting / listening
    virtual Result<void> start() = 0;
    virtual void stop() = 0;

    // Send bytes (non-blocking; returns error if buffer full)
    virtual Result<void> send(const char *data, std::size_t len) = 0;
    virtual Result<void> send(const std::string &data) { return send(data.data(), data.size()); }

    [[nodiscard]] virtual bool is_connected() const noexcept = 0;

    // Protocol reported by the concrete transport
    [[nodiscard]] virtual TransportProtocol protocol() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// TcpTransportConfig
// ---------------------------------------------------------------------------
struct TcpTransportConfig {
    std::string host; // for initiators
    std::uint16_t port = 0;
    bool initiator = true;
    bool tls = false; // TLS 1.3 via OpenSSL (future)
    std::string tls_cert;
    std::string tls_key;
    std::string tls_ca;
    std::size_t recv_buffer_size = 65536;
    std::size_t send_buffer_size = 65536;
    int backlog = 128; // for acceptors
};

// ---------------------------------------------------------------------------
// UdpTransportConfig
//
// UDP is suited for stateless market-data feeds (unicast or multicast).
// FIX session-layer sequencing still applies when using UDP, but the
// transport itself is inherently unreliable – the application must decide
// whether that is acceptable (e.g. one-way price broadcast).
// ---------------------------------------------------------------------------
struct UdpTransportConfig {
    // Local bind address (empty = INADDR_ANY).
    std::string bind_host;
    std::uint16_t bind_port = 0;

    // Remote address used by initiators when sending.
    std::string remote_host;
    std::uint16_t remote_port = 0;

    // Multicast settings (ignored when multicast = false).
    bool multicast = false;
    std::string multicast_group; // e.g. "239.0.0.1"
    std::string multicast_iface; // local interface IP for join/send
    std::uint8_t multicast_ttl = 8;

    // initiator = sender (write-only direction matters for routing),
    // acceptor  = receiver (bind and listen for datagrams).
    bool initiator = true;

    std::size_t recv_buffer_size = 65536;
};

// ---------------------------------------------------------------------------
// LoadBalancer backend endpoint descriptor
// ---------------------------------------------------------------------------
struct BackendEndpoint {
    std::string host;
    std::uint16_t port = 0;
    TransportProtocol protocol = TransportProtocol::TCP;
    int weight = 1; // relative weight for weighted round-robin
};

// ---------------------------------------------------------------------------
// Load-balancer strategy
// ---------------------------------------------------------------------------
enum class LbStrategy : std::uint8_t {
    RoundRobin = 0, // cycle through all healthy backends
    Failover = 1,   // always use primary; fall back only when it is down
    WeightedRR = 2, // weighted round-robin proportional to BackendEndpoint::weight
};

// ---------------------------------------------------------------------------
// LoadBalancerConfig
// ---------------------------------------------------------------------------
struct LoadBalancerConfig {
    std::vector<BackendEndpoint> backends;
    LbStrategy strategy = LbStrategy::RoundRobin;
    std::size_t recv_buffer_size = 65536;
    // Interval between health-check reconnect attempts (ms).
    int reconnect_interval_ms = 5000;
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
std::unique_ptr<ITransport> make_tcp_transport(TcpTransportConfig cfg);
std::unique_ptr<ITransport> make_udp_transport(UdpTransportConfig cfg);
std::unique_ptr<ITransport> make_load_balancer(LoadBalancerConfig cfg);

/// Convenience: choose TCP or UDP automatically from a single protocol flag.
/// For FIX order-management use TCP; for market-data feeds use UDP.
inline std::unique_ptr<ITransport> make_transport(TransportProtocol proto, const std::string &host,
                                                  std::uint16_t port, bool initiator = true) {
    if (proto == TransportProtocol::UDP) {
        UdpTransportConfig cfg;
        if (initiator) {
            cfg.remote_host = host;
            cfg.remote_port = port;
            cfg.initiator = true;
        } else {
            cfg.bind_host = host;
            cfg.bind_port = port;
            cfg.initiator = false;
        }
        return make_udp_transport(std::move(cfg));
    }
    // Default: TCP
    TcpTransportConfig cfg;
    cfg.host = host;
    cfg.port = port;
    cfg.initiator = initiator;
    return make_tcp_transport(std::move(cfg));
}

} // namespace fix
