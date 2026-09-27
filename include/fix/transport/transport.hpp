#pragma once
// =============================================================================
// FIX Protocol Engine - Transport abstraction
// =============================================================================
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "../core/types.hpp"

namespace fix {

// ---------------------------------------------------------------------------
// Transport events
// ---------------------------------------------------------------------------
using ConnectedCallback = std::function<void()>;
using DisconnectedCallback = std::function<void(std::string_view reason)>;
using DataCallback = std::function<void(const char *data, std::size_t len)>;
using ErrorCallback = std::function<void(std::error_code ec)>;

namespace detail {
// Set by ITransport implementations at the entry of their IO thread; holds
// the owning transport instance. thread_local storage expires with the
// thread, so no explicit clear is needed and no other thread can ever read
// a stale owner back.
inline thread_local const void *current_transport_io = nullptr;
} // namespace detail

// True when the calling thread is *some* transport's IO thread. Engine uses
// this before joining the timer thread: the timer may itself be joining a
// transport IO thread (reap), so joining it back from one would deadlock.
[[nodiscard]] inline bool on_transport_io_thread() noexcept {
    return detail::current_transport_io != nullptr;
}

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

    // Close the current connection (if any) but keep the transport running:
    // acceptors return to accept(), initiators may reconnect per config.
    // Called by the session layer on protocol violations (e.g. CompID
    // mismatch) and heartbeat timeouts – without it a hostile peer would pin
    // the acceptor's event loop forever. Default no-op so out-of-tree
    // implementations keep compiling; TcpTransport overrides it.
    virtual void disconnect() {}

    // True when the calling thread *is* this transport's IO thread. Engine
    // uses it to avoid joining the IO thread from itself (which would abort);
    // such transports are reaped by another thread instead.
    [[nodiscard]] virtual bool on_io_thread() const noexcept { return false; }

    // Send bytes (non-blocking; returns error if buffer full)
    virtual Result<void> send(const char *data, std::size_t len) = 0;
    virtual Result<void> send(const std::string &data) { return send(data.data(), data.size()); }

    [[nodiscard]] virtual bool is_connected() const noexcept = 0;
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
    // Applied as SO_SNDBUF on every connection socket (accepted + initiated).
    // Explicitly setting it disables kernel send autotuning — pick a value
    // that covers the peer's window for your link. 0 leaves the kernel
    // default/autotuning in place.
    std::size_t send_buffer_size = 65536;
    int backlog = 128; // for acceptors

    // Initiator reconnect backoff: starts at reconnect_delay, doubles after
    // each failed attempt up to reconnect_delay_max, and resets to the initial
    // delay after a connection was established (and later dropped).
    std::chrono::milliseconds reconnect_delay{5000};
    std::chrono::milliseconds reconnect_delay_max{60000};
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<ITransport> make_tcp_transport(TcpTransportConfig cfg);

} // namespace fix
