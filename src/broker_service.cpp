#include "broker_service.h"

#include "adi_server.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <thread>

namespace nvlink::broker {
namespace {

std::vector<double> decode_payload_as_doubles(const std::vector<uint8_t>& payload) {
    if (payload.size() % sizeof(double) != 0) {
        throw NVLinkError("payload must be a packed array of doubles");
    }
    std::vector<double> values(payload.size() / sizeof(double), 0.0);
    std::memcpy(values.data(), payload.data(), payload.size());
    return values;
}

std::vector<uint8_t> encode_doubles_as_payload(const std::vector<double>& values) {
    std::vector<uint8_t> payload(values.size() * sizeof(double));
    if (!payload.empty()) {
        std::memcpy(payload.data(), values.data(), payload.size());
    }
    return payload;
}

} // namespace

BrokerService::BrokerService(const BrokerServiceConfig& config)
    : config_(config) {
    if (!config_.force_cpu_fallback && get_gpu_count() > 0) {
        topology_ = std::make_unique<GpuTopology>(GpuTopology::detect());
        placer_ = std::make_unique<Placer>(*topology_, config_.backlog_threshold);
        use_gpu_placement_ = true;
        gpu_inflight_.assign(static_cast<std::size_t>(topology_->num_gpus()), 0);
    }
}

BrokerResponse BrokerService::handle_request(const BrokerRequest& request) {
    BrokerResponse response;
    response.task_id = request.task_id;

    const auto started = std::chrono::steady_clock::now();
    std::string validation_error;
    if (!validate_request(request, &validation_error, config_.protocol_limits)) {
        response.status = TaskStatus::Rejected;
        response.error = validation_error;
        return response;
    }

    int selected_gpu = -1;
    bool incremented_inflight = false;

    try {
        if (config_.simulated_latency_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.simulated_latency_ms));
        }

        if (use_gpu_placement_) {
            const int client_id = get_or_assign_client_id(request.source);
            selected_gpu = placer_->place(client_id, [this](int gpu) {
                std::lock_guard<std::mutex> lock(inflight_mutex_);
                return gpu_inflight_[static_cast<std::size_t>(gpu)];
            });

            {
                std::lock_guard<std::mutex> lock(inflight_mutex_);
                std::size_t& inflight = gpu_inflight_[static_cast<std::size_t>(selected_gpu)];
                ++inflight;
                incremented_inflight = true;
                if (inflight > config_.max_inflight_per_gpu) {
                    --inflight;
                    incremented_inflight = false;
                    response.status = TaskStatus::Rejected;
                    response.error = "selected GPU queue is at capacity";
                    response.selected_gpu = selected_gpu;
                    return response;
                }
            }
        }

        const std::vector<double> input = decode_payload_as_doubles(request.payload);
        const int compute_gpu = (selected_gpu >= 0) ? selected_gpu : 0;
        const std::vector<double> output = nvlink::adi::default_gpu_compute(input, compute_gpu);

        response.status = TaskStatus::Ok;
        response.selected_gpu = selected_gpu;
        response.result = encode_doubles_as_payload(output);
    } catch (const std::exception& ex) {
        response.status = TaskStatus::Error;
        response.error = ex.what();
        response.selected_gpu = selected_gpu;
    }

    if (incremented_inflight && selected_gpu >= 0) {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        std::size_t& inflight = gpu_inflight_[static_cast<std::size_t>(selected_gpu)];
        if (inflight > 0) {
            --inflight;
        }
    }

    const auto ended = std::chrono::steady_clock::now();
    response.latency_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(ended - started).count());
    return response;
}

bool BrokerService::has_gpu_topology() const noexcept {
    return use_gpu_placement_;
}

int BrokerService::gpu_count() const noexcept {
    if (topology_ == nullptr) {
        return 0;
    }
    return topology_->num_gpus();
}

int BrokerService::get_or_assign_client_id(const std::string& source) {
    std::lock_guard<std::mutex> lock(client_mutex_);
    const auto it = source_to_client_.find(source);
    if (it != source_to_client_.end()) {
        return it->second;
    }

    const int client_id = next_client_id_.fetch_add(1, std::memory_order_relaxed);
    source_to_client_.emplace(source, client_id);
    placer_->assign_home(client_id);
    return client_id;
}

} // namespace nvlink::broker
