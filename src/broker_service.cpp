#include "broker_service.h"

#include "adi_server.h"
#include "geospatial_frame.h"
#include "media_stream.h"
#include "wasm_sandbox.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <thread>

namespace nvlink::broker {
namespace {

std::vector<double> decode_payload_as_doubles(const std::vector<uint8_t>& payload) {
    if (payload.size() % sizeof(double) != 0) {
        throw NVLinkError("payload must be a packed array of doubles");
    }
    std::vector<double> values(payload.size() / sizeof(double), 0.0);
    static_assert(sizeof(double) == sizeof(uint64_t), "inference payload requires binary64 doubles");
    static_assert(std::numeric_limits<double>::is_iec559, "inference payload requires IEEE-754 doubles");
    for (std::size_t i = 0; i < values.size(); ++i) {
        uint64_t bits = 0;
        for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
            bits = (bits << 8) | payload[i * sizeof(double) + byte];
        }
        std::memcpy(&values[i], &bits, sizeof(bits));
    }
    return values;
}

std::vector<uint8_t> encode_doubles_as_payload(const std::vector<double>& values) {
    std::vector<uint8_t> payload(values.size() * sizeof(double));
    static_assert(sizeof(double) == sizeof(uint64_t), "inference payload requires binary64 doubles");
    static_assert(std::numeric_limits<double>::is_iec559, "inference payload requires IEEE-754 doubles");
    for (std::size_t i = 0; i < values.size(); ++i) {
        uint64_t bits = 0;
        std::memcpy(&bits, &values[i], sizeof(bits));
        for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
            payload[i * sizeof(double) + byte] =
                static_cast<uint8_t>(bits >> ((sizeof(double) - byte - 1) * 8));
        }
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

    if (config_.wasm_manager != nullptr) {
        wasm_manager_ = config_.wasm_manager;
    } else {
        nvlink::wasm::WasmSandboxManager::GpuSelectorFn selector = nullptr;
        if (use_gpu_placement_) {
            selector = [this](const std::string& source, int32_t target_gpu) -> int32_t {
                if (target_gpu >= 0) {
                    return target_gpu;
                }
                const int client_id = get_or_assign_client_id(source);
                return placer_->place(client_id, [this](int gpu) {
                    std::lock_guard<std::mutex> lock(inflight_mutex_);
                    return gpu_inflight_[static_cast<std::size_t>(gpu)];
                });
            };
        }
        wasm_manager_ = std::make_shared<nvlink::wasm::WasmSandboxManager>(
            config_.force_cpu_fallback || !use_gpu_placement_, selector);
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

    const auto deadline = started + std::chrono::milliseconds(request.timeout_ms);
    const auto finish_response = [&](BrokerResponse result) {
        const auto ended = std::chrono::steady_clock::now();
        result.latency_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(ended - started).count());
        if (ended >= deadline) {
            result.status = TaskStatus::Timeout;
            result.result.clear();
            result.error = "request timed out";
        }
        return result;
    };

    if (config_.simulated_latency_ms > 0) {
        const auto simulated_end = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(config_.simulated_latency_ms);
        std::this_thread::sleep_until(std::min(simulated_end, deadline));
        if (std::chrono::steady_clock::now() >= deadline) {
            return finish_response(response);
        }
    }

    if (request.kind == nvlink::geospatial::kGeospatialFrameKind) {
        return finish_response(handle_geospatial_frame_request(request));
    }

    if (request.kind == nvlink::media::kMediaStreamKind) {
        return finish_response(handle_media_stream_request(request));
    }

    if (request.kind == nvlink::wasm::kWasmTaskKind) {
        return finish_response(handle_wasm_task_request(request));
    }

    if (request.kind == nvlink::wasm::kWasmLifecycleKind ||
        request.kind == nvlink::wasm::kWasmLifecyclePrepareKind ||
        request.kind == nvlink::wasm::kWasmLifecycleDrainKind ||
        request.kind == nvlink::wasm::kWasmLifecycleReleaseKind ||
        request.kind == nvlink::wasm::kWasmLifecycleQueryKind) {
        return finish_response(handle_wasm_lifecycle_request(request));
    }

    int selected_gpu = -1;
    bool incremented_inflight = false;

    try {
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
                    return finish_response(std::move(response));
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

    return finish_response(std::move(response));
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

BrokerResponse BrokerService::handle_geospatial_frame_request(const BrokerRequest& request) const {
    BrokerResponse response;
    response.task_id = request.task_id;

    nvlink::geospatial::GeospatialFrame frame;
    std::string decode_error;
    if (!nvlink::geospatial::decode_nxr1_frame(
            request.payload, &frame, &decode_error, config_.protocol_limits.max_payload_bytes)) {
        response.status = TaskStatus::Rejected;
        response.error = decode_error;
        return response;
    }

    // Acknowledge the validated frame by echoing it back, letting callers
    // confirm an end-to-end round trip without additional side effects.
    std::string encode_error;
    std::vector<uint8_t> encoded;
    if (!nvlink::geospatial::encode_nxr1_frame(
            frame, &encoded, &encode_error, config_.protocol_limits.max_payload_bytes)) {
        response.status = TaskStatus::Error;
        response.error = encode_error;
        return response;
    }

    response.status = TaskStatus::Ok;
    response.result = std::move(encoded);
    return response;
}

BrokerResponse BrokerService::handle_media_stream_request(const BrokerRequest& request) const {
    BrokerResponse response;
    response.task_id = request.task_id;

    nvlink::media::MediaStreamFrame frame;
    std::string decode_error;
    if (!nvlink::media::decode_media_stream_frame(
            request.payload, &frame, &decode_error, config_.protocol_limits.max_payload_bytes,
            config_.protocol_limits.max_string_bytes)) {
        response.status = TaskStatus::Rejected;
        response.error = decode_error;
        return response;
    }

    std::string encode_error;
    std::vector<uint8_t> encoded;
    if (!nvlink::media::encode_media_stream_frame(
            frame, &encoded, &encode_error, config_.protocol_limits.max_payload_bytes,
            config_.protocol_limits.max_string_bytes)) {
        response.status = TaskStatus::Error;
        response.error = encode_error;
        return response;
    }

    response.status = TaskStatus::Ok;
    response.result = std::move(encoded);
    return response;
}

BrokerResponse BrokerService::handle_wasm_task_request(const BrokerRequest& request) {
    BrokerResponse response;
    response.task_id = request.task_id;

    nvlink::wasm::WasmTaskRequest wasm_req;
    std::string decode_error;
    nvlink::wasm::WasmLimits limits;
    limits.max_payload_bytes = config_.protocol_limits.max_payload_bytes;
    limits.max_frame_bytes = config_.protocol_limits.max_frame_bytes;
    limits.max_string_bytes = config_.protocol_limits.max_string_bytes;

    if (!nvlink::wasm::decode_wasm_task_request(request.payload, &wasm_req, &decode_error, limits)) {
        response.status = TaskStatus::Rejected;
        response.error = "failed to decode WASM task request: " + decode_error;
        return response;
    }

    const nvlink::wasm::WasmTaskResponse wasm_resp = wasm_manager_->handle_task(wasm_req);

    std::string encode_error;
    std::vector<uint8_t> encoded_resp;
    if (!nvlink::wasm::encode_wasm_task_response(wasm_resp, &encoded_resp, &encode_error, limits)) {
        response.status = TaskStatus::Error;
        response.error = "failed to encode WASM task response: " + encode_error;
        return response;
    }

    switch (wasm_resp.status) {
        case nvlink::wasm::WasmTaskStatus::Ok:
            response.status = TaskStatus::Ok;
            break;
        case nvlink::wasm::WasmTaskStatus::Timeout:
            response.status = TaskStatus::Timeout;
            break;
        case nvlink::wasm::WasmTaskStatus::Rejected:
        case nvlink::wasm::WasmTaskStatus::ModuleDraining:
        case nvlink::wasm::WasmTaskStatus::ModuleNotFound:
        case nvlink::wasm::WasmTaskStatus::VersionMismatch:
            response.status = TaskStatus::Rejected;
            break;
        default:
            response.status = TaskStatus::Error;
            break;
    }

    response.selected_gpu = wasm_resp.selected_gpu;
    response.result = std::move(encoded_resp);
    response.error = wasm_resp.error;
    return response;
}

BrokerResponse BrokerService::handle_wasm_lifecycle_request(const BrokerRequest& request) {
    BrokerResponse response;
    response.task_id = request.task_id;

    nvlink::wasm::WasmLifecycleRequest wasm_req;
    std::string decode_error;
    nvlink::wasm::WasmLimits limits;
    limits.max_payload_bytes = config_.protocol_limits.max_payload_bytes;
    limits.max_frame_bytes = config_.protocol_limits.max_frame_bytes;
    limits.max_string_bytes = config_.protocol_limits.max_string_bytes;

    if (!nvlink::wasm::decode_wasm_lifecycle_request(request.payload, &wasm_req, &decode_error, limits)) {
        response.status = TaskStatus::Rejected;
        response.error = "failed to decode WASM lifecycle request: " + decode_error;
        return response;
    }

    if (request.kind == nvlink::wasm::kWasmLifecyclePrepareKind) {
        wasm_req.action = nvlink::wasm::WasmLifecycleAction::Prepare;
    } else if (request.kind == nvlink::wasm::kWasmLifecycleDrainKind) {
        wasm_req.action = nvlink::wasm::WasmLifecycleAction::Drain;
    } else if (request.kind == nvlink::wasm::kWasmLifecycleReleaseKind) {
        wasm_req.action = nvlink::wasm::WasmLifecycleAction::Release;
    } else if (request.kind == nvlink::wasm::kWasmLifecycleQueryKind) {
        wasm_req.action = nvlink::wasm::WasmLifecycleAction::Query;
    }

    const nvlink::wasm::WasmLifecycleResponse wasm_resp = wasm_manager_->handle_lifecycle(wasm_req);

    std::string encode_error;
    std::vector<uint8_t> encoded_resp;
    if (!nvlink::wasm::encode_wasm_lifecycle_response(wasm_resp, &encoded_resp, &encode_error, limits)) {
        response.status = TaskStatus::Error;
        response.error = "failed to encode WASM lifecycle response: " + encode_error;
        return response;
    }

    switch (wasm_resp.status) {
        case nvlink::wasm::WasmLifecycleStatus::Ok:
            response.status = TaskStatus::Ok;
            break;
        case nvlink::wasm::WasmLifecycleStatus::Rejected:
        case nvlink::wasm::WasmLifecycleStatus::NotFound:
        case nvlink::wasm::WasmLifecycleStatus::Busy:
            response.status = TaskStatus::Rejected;
            break;
        default:
            response.status = TaskStatus::Error;
            break;
    }

    response.selected_gpu = wasm_resp.assigned_gpu;
    response.result = std::move(encoded_resp);
    response.error = wasm_resp.error;
    return response;
}

std::shared_ptr<nvlink::wasm::WasmSandboxManager> BrokerService::wasm_manager() const noexcept {
    return wasm_manager_;
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
