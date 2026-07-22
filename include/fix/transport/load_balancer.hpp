#pragma once
// =============================================================================
// FIX Protocol Engine - Load Balancer Transport
//
// LoadBalancerTransport implements ITransport and manages a pool of backend
// transports (TCP or UDP).  It provides:
//
//   * Round-robin  – cycles through all healthy backends on every send().
//   * Failover     – sticks to the primary (index 0); promotes the next
//                    healthy backend only when the primary goes down.
//   * WeightedRR   – like round-robin but proportional to BackendEndpoint::weight.
//
// All backends receive data independently; the on_data callback fires for
// whichever backend delivers bytes first (first-response-wins for any given
// message).  This matches common FIX-over-multi-venue patterns where the same
// market-data stream is available on multiple endpoints for redundancy.
//
// on_connected  – fires when at least one backend comes up.
// on_disconnected – fires when ALL backends go down.
// =============================================================================
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "transport.hpp"

namespace fix {

class LoadBalancerTransport : public ITransport {
public:
    explicit LoadBalancerTransport(LoadBalancerConfig cfg);
    ~LoadBalancerTransport() override;

    void set_on_connected(ConnectedCallback cb) override { on_connected_ = std::move(cb); }
    void set_on_disconnected(DisconnectedCallback cb) override { on_disconnected_ = std::move(cb); }
    void set_on_data(DataCallback cb) override { on_data_ = std::move(cb); }
    void set_on_error(ErrorCallback cb) override { on_error_ = std::move(cb); }

    Result<void> start() override;
    void stop() override;
    Result<void> send(const char *data, std::size_t len) override;

    [[nodiscard]] bool is_connected() const noexcept override {
        return connected_count_.load(std::memory_order_acquire) > 0;
    }
    [[nodiscard]] TransportProtocol protocol() const noexcept override {
        return TransportProtocol::TCP; // representative; backends may differ
    }

    // Returns the number of currently healthy backends.
    [[nodiscard]] std::size_t healthy_count() const noexcept {
        return static_cast<std::size_t>(connected_count_.load(std::memory_order_acquire));
    }

    // Returns total number of configured backends.
    [[nodiscard]] std::size_t backend_count() const noexcept { return backends_.size(); }

private:
    LoadBalancerConfig cfg_;

    ConnectedCallback on_connected_;
    DisconnectedCallback on_disconnected_;
    DataCallback on_data_;
    ErrorCallback on_error_;

    struct BackendState {
        std::unique_ptr<ITransport> transport;
        std::atomic<bool> connected{false};
        std::string host;
        std::uint16_t port = 0;
        TransportProtocol proto = TransportProtocol::TCP;
        int weight = 1;
    };

    std::vector<std::unique_ptr<BackendState>> backends_;
    mutable std::mutex state_mutex_;

    std::atomic<int> connected_count_{0};

    // For round-robin / weighted-rr cursor
    std::atomic<std::size_t> rr_cursor_{0};

    // For WeightedRR: pre-expanded index list (e.g. weight 2 → index appears twice)
    std::vector<std::size_t> weighted_sequence_;
    std::atomic<std::size_t> ww_cursor_{0};

    void build_weighted_sequence();

    // Select the next backend index to send to; returns SIZE_MAX on failure.
    std::size_t pick_backend();

    void on_backend_connected(std::size_t idx);
    void on_backend_disconnected(std::size_t idx, std::string_view reason);
};

} // namespace fix
