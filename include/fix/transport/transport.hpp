#pragma once
// =============================================================================
// FIX Protocol Engine - Transport abstraction
// =============================================================================
#include "../core/types.hpp"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <cstddef>

namespace fix {

// ---------------------------------------------------------------------------
// Transport events
// ---------------------------------------------------------------------------
using ConnectedCallback    = std::function<void()>;
using DisconnectedCallback = std::function<void(std::string_view reason)>;
using DataCallback         = std::function<void(const char* data, std::size_t len)>;
using ErrorCallback        = std::function<void(std::error_code ec)>;

// ---------------------------------------------------------------------------
// ITransport - abstract network transport
// ---------------------------------------------------------------------------
class ITransport {
public:
    virtual ~ITransport() = default;

    // Set event callbacks
    virtual void set_on_connected(ConnectedCallback cb)       = 0;
    virtual void set_on_disconnected(DisconnectedCallback cb) = 0;
    virtual void set_on_data(DataCallback cb)                 = 0;
    virtual void set_on_error(ErrorCallback cb)               = 0;

    // Start connecting / listening
    virtual Result<void> start() = 0;
    virtual void         stop()  = 0;

    // Send bytes (non-blocking; returns error if buffer full)
    virtual Result<void> send(const char* data, std::size_t len) = 0;
    virtual Result<void> send(const std::string& data) {
        return send(data.data(), data.size());
    }

    [[nodiscard]] virtual bool is_connected() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// TcpTransportConfig
// ---------------------------------------------------------------------------
struct TcpTransportConfig {
    std::string host;           // for initiators
    std::uint16_t port = 0;
    bool initiator = true;
    bool tls       = false;     // TLS 1.3 via OpenSSL (future)
    std::string tls_cert;
    std::string tls_key;
    std::string tls_ca;
    std::size_t recv_buffer_size = 65536;
    std::size_t send_buffer_size = 65536;
    int         backlog          = 128;   // for acceptors
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<ITransport> make_tcp_transport(TcpTransportConfig cfg);

} // namespace fix
