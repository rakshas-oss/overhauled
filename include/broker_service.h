#pragma once

#include "broker_protocol.h"
#include "nvlink_placement.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nvlink::broker {

struct BrokerServiceConfig {
    std::size_t backlog_threshold = 32;
    std::size_t max_inflight_per_gpu = 1024;
    bool force_cpu_fallback = false;
    uint32_t simulated_latency_ms = 0;
    ProtocolLimits protocol_limits{};
};

class BrokerService {
public:
    explicit BrokerService(const BrokerServiceConfig& config = {});

    BrokerResponse handle_request(const BrokerRequest& request);

    bool has_gpu_topology() const noexcept;
    int gpu_count() const noexcept;

private:
    int get_or_assign_client_id(const std::string& source);

    BrokerServiceConfig config_;
    bool use_gpu_placement_ = false;
    std::unique_ptr<GpuTopology> topology_;
    std::unique_ptr<Placer> placer_;
    std::vector<std::size_t> gpu_inflight_;
    mutable std::mutex inflight_mutex_;

    mutable std::mutex client_mutex_;
    std::unordered_map<std::string, int> source_to_client_;
    std::atomic<int> next_client_id_{1};
};

} // namespace nvlink::broker
