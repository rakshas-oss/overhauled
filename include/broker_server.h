// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors

#pragma once

#include "broker_service.h"
#include "worker_scheduler.h"

#include <atomic>
#include <memory>

namespace nvlink::broker {

// The listener uses one blocking session-I/O thread per admitted client.
// Keep the total thread count bounded even if an operator misconfigures it.
inline constexpr std::size_t kMaximumBrokerClientSessions = 1024;

struct BrokerServerConfig {
    int port = 9090;
    // Bind to loopback when using an external mTLS terminator (for example stunnel).
    std::string bind_address = "0.0.0.0";
    // Require the plaintext broker listener to remain loopback-only behind an mTLS terminator.
    bool require_mtls_proxy = false;
    int socket_timeout_ms = 5000;
    std::size_t max_clients = 128;
    std::size_t max_inflight_per_client = 32;
    // 0 derives a safe process-wide cap from max_clients * max_inflight_per_client,
    // capped at 65,536 outstanding requests.
    std::size_t max_outstanding_requests = 0;
    // Accounts queued/executing payload plus request metadata; protects heap use during bursts.
    std::size_t max_inflight_memory_bytes = 64 * 1024 * 1024;
    // Legacy total worker budget. Dedicated class workers are reserved
    // first, optional elastic workers are carved out next, and the remainder
    // is assigned to Throughput (at least one worker is retained).
    std::size_t worker_threads = 0; // 0 = automatic sizing
    std::size_t control_worker_threads = 1;
    std::size_t realtime_spatial_worker_threads = 1;
    std::size_t throughput_worker_threads = 0; // 0 = use legacy/automatic budget
    std::size_t exact_cpu_worker_threads = 1;
    std::size_t inference_worker_threads = 1;
    // Optional additional worker slots. Zero keeps dedicated-only sizing.
    // Elastic workers are capped at 4,096; all workers are capped at 16,384
    // and further bounded by the process-visible CPU topology. Elastic work
    // is limited to RealtimeSpatial, Throughput and Inference.
    std::size_t elastic_worker_threads = 0;
    std::array<std::size_t, kWorkerClassCount> elastic_max_workers_per_class{{0, 1, 4, 0, 2}};
    bool verbose = false;
    BrokerServiceConfig service_config{};
};

class BrokerServer {
public:
    explicit BrokerServer(const BrokerServerConfig& config = {});
    ~BrokerServer();

    void run();
    void stop() noexcept;
    bool is_running() const noexcept;

    BrokerServer(const BrokerServer&) = delete;
    BrokerServer& operator=(const BrokerServer&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace nvlink::broker
