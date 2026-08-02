// =============================================================================
// FIX Protocol Engine - LoadBalancerTransport implementation
// =============================================================================
#include "fix/transport/load_balancer.hpp"

#include "fix/transport/tcp_transport.hpp"
#include "fix/transport/udp_transport.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace fix {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
LoadBalancerTransport::LoadBalancerTransport(LoadBalancerConfig cfg)
    : cfg_(std::move(cfg)) {
    backends_.reserve(cfg_.backends.size());

    for (const auto &ep : cfg_.backends) {
        auto state = std::make_unique<BackendState>();
        state->host = ep.host;
        state->port = ep.port;
        state->proto = ep.protocol;
        state->weight = ep.weight > 0 ? ep.weight : 1;

        // Create the underlying transport for this backend.
        if (ep.protocol == TransportProtocol::UDP) {
            UdpTransportConfig ucfg;
            ucfg.remote_host = ep.host;
            ucfg.remote_port = ep.port;
            ucfg.initiator = true;
            ucfg.recv_buffer_size = cfg_.recv_buffer_size;
            state->transport = make_udp_transport(std::move(ucfg));
        } else {
            TcpTransportConfig tcfg;
            tcfg.host = ep.host;
            tcfg.port = ep.port;
            tcfg.initiator = true;
            tcfg.recv_buffer_size = cfg_.recv_buffer_size;
            state->transport = make_tcp_transport(std::move(tcfg));
        }

        backends_.push_back(std::move(state));
    }

    build_weighted_sequence();
}

LoadBalancerTransport::~LoadBalancerTransport() {
    stop();
}

// ---------------------------------------------------------------------------
// Build the weighted round-robin index sequence.
// Example: weights [2, 1] → sequence [0, 0, 1]
// ---------------------------------------------------------------------------
void LoadBalancerTransport::build_weighted_sequence() {
    weighted_sequence_.clear();
    for (std::size_t i = 0; i < backends_.size(); ++i) {
        for (int w = 0; w < backends_[i]->weight; ++w) {
            weighted_sequence_.push_back(i);
        }
    }
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------
Result<void> LoadBalancerTransport::start() {
    if (backends_.empty())
        return make_unexpected(ErrorCode::TransportError);

    for (std::size_t i = 0; i < backends_.size(); ++i) {
        auto *state = backends_[i].get();
        const std::size_t idx = i;

        state->transport->set_on_connected([this, idx] { on_backend_connected(idx); });
        state->transport->set_on_disconnected(
            [this, idx](std::string_view reason) { on_backend_disconnected(idx, reason); });
        state->transport->set_on_data([this](const char *data, std::size_t len) {
            if (on_data_)
                on_data_(data, len);
        });
        state->transport->set_on_error([this](std::error_code ec) {
            if (on_error_)
                on_error_(ec);
        });

        state->transport->start();
    }

    return {};
}

void LoadBalancerTransport::stop() {
    for (auto &state : backends_) {
        state->transport->stop();
        state->connected.store(false, std::memory_order_release);
    }
    connected_count_.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Backend event handlers
// ---------------------------------------------------------------------------
void LoadBalancerTransport::on_backend_connected(std::size_t idx) {
    backends_[idx]->connected.store(true, std::memory_order_release);
    int prev = connected_count_.fetch_add(1, std::memory_order_acq_rel);
    if (prev == 0 && on_connected_) {
        // First backend came up – notify the session layer.
        on_connected_();
    }
}

void LoadBalancerTransport::on_backend_disconnected(std::size_t idx, std::string_view reason) {
    bool was_connected = backends_[idx]->connected.exchange(false, std::memory_order_acq_rel);
    if (was_connected) {
        int remaining = connected_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining <= 0 && on_disconnected_) {
            on_disconnected_(reason);
        }
    }
}

// ---------------------------------------------------------------------------
// Backend selection
// ---------------------------------------------------------------------------
std::size_t LoadBalancerTransport::pick_backend() {
    if (backends_.empty())
        return std::numeric_limits<std::size_t>::max();

    switch (cfg_.strategy) {
    // ------------------------------------------------------------------
    case LbStrategy::Failover: {
        // Primary first (index 0), then fall through to others.
        for (std::size_t i = 0; i < backends_.size(); ++i) {
            if (backends_[i]->connected.load(std::memory_order_acquire))
                return i;
        }
        return std::numeric_limits<std::size_t>::max();
    }

    // ------------------------------------------------------------------
    case LbStrategy::WeightedRR: {
        if (weighted_sequence_.empty())
            return std::numeric_limits<std::size_t>::max();

        // Try each slot in the weighted sequence starting from the cursor.
        std::size_t sz = weighted_sequence_.size();
        for (std::size_t attempt = 0; attempt < sz; ++attempt) {
            std::size_t pos = ww_cursor_.fetch_add(1, std::memory_order_relaxed) % sz;
            std::size_t idx = weighted_sequence_[pos];
            if (backends_[idx]->connected.load(std::memory_order_acquire))
                return idx;
        }
        return std::numeric_limits<std::size_t>::max();
    }

    // ------------------------------------------------------------------
    case LbStrategy::RoundRobin:
    default: {
        std::size_t sz = backends_.size();
        for (std::size_t attempt = 0; attempt < sz; ++attempt) {
            std::size_t idx = rr_cursor_.fetch_add(1, std::memory_order_relaxed) % sz;
            if (backends_[idx]->connected.load(std::memory_order_acquire))
                return idx;
        }
        return std::numeric_limits<std::size_t>::max();
    }
    }
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------
Result<void> LoadBalancerTransport::send(const char *data, std::size_t len) {
    std::size_t idx = pick_backend();
    if (idx == std::numeric_limits<std::size_t>::max())
        return make_unexpected(ErrorCode::TransportError);

    return backends_[idx]->transport->send(data, len);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<ITransport> make_load_balancer(LoadBalancerConfig cfg) {
    return std::make_unique<LoadBalancerTransport>(std::move(cfg));
}

} // namespace fix
