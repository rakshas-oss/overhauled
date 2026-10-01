#include "wasm_sandbox.h"
#include "adi_server.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>

namespace nvlink::wasm {
namespace {

void append_bytes(std::vector<uint8_t>* out, const void* data, std::size_t size) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    out->insert(out->end(), ptr, ptr + size);
}

void append_be16(std::vector<uint8_t>* out, uint16_t value) {
    const uint16_t be = htons(value);
    append_bytes(out, &be, sizeof(be));
}

void append_be32(std::vector<uint8_t>* out, uint32_t value) {
    const uint32_t be = htonl(value);
    append_bytes(out, &be, sizeof(be));
}

void append_be64(std::vector<uint8_t>* out, uint64_t value) {
    const uint32_t hi = htonl(static_cast<uint32_t>(value >> 32));
    const uint32_t lo = htonl(static_cast<uint32_t>(value & 0xffffffffULL));
    append_bytes(out, &hi, sizeof(hi));
    append_bytes(out, &lo, sizeof(lo));
}

bool append_string(std::vector<uint8_t>* out,
                   const std::string& value,
                   const WasmLimits& limits,
                   std::string* error) {
    if (value.size() > limits.max_string_bytes || value.size() > std::numeric_limits<uint16_t>::max()) {
        if (error != nullptr) {
            *error = "string length exceeds maximum allowed limit";
        }
        return false;
    }
    append_be16(out, static_cast<uint16_t>(value.size()));
    append_bytes(out, value.data(), value.size());
    return true;
}

bool append_blob(std::vector<uint8_t>* out,
                 const std::vector<uint8_t>& blob,
                 const WasmLimits& limits,
                 std::string* error) {
    if (blob.size() > limits.max_payload_bytes || blob.size() > std::numeric_limits<uint32_t>::max()) {
        if (error != nullptr) {
            *error = "payload blob exceeds maximum allowed limit";
        }
        return false;
    }
    append_be32(out, static_cast<uint32_t>(blob.size()));
    append_bytes(out, blob.data(), blob.size());
    return true;
}

bool append_buffer_descriptors(std::vector<uint8_t>* out,
                              const std::vector<WasmBufferDescriptor>& buffers,
                              const WasmLimits& limits,
                              std::string* error) {
    if (buffers.size() > limits.max_buffers || buffers.size() > std::numeric_limits<uint16_t>::max()) {
        if (error != nullptr) {
            *error = "buffer descriptor count exceeds maximum allowed limit";
        }
        return false;
    }
    append_be16(out, static_cast<uint16_t>(buffers.size()));
    for (const auto& buf : buffers) {
        append_be32(out, buf.buffer_id);
        append_be32(out, buf.flags);
        append_be64(out, buf.offset);
        append_be64(out, buf.length);
        if (!append_string(out, buf.name, limits, error)) {
            return false;
        }
    }
    return true;
}

bool take(std::size_t need, std::size_t size, std::size_t* offset, std::string* error) {
    if (*offset > size || need > size - *offset) {
        if (error != nullptr) {
            *error = "frame truncated or incomplete";
        }
        return false;
    }
    return true;
}

bool read_be16(const std::vector<uint8_t>& frame, std::size_t* offset, uint16_t* out, std::string* error) {
    if (!take(sizeof(uint16_t), frame.size(), offset, error)) {
        return false;
    }
    uint16_t be = 0;
    std::memcpy(&be, frame.data() + *offset, sizeof(be));
    *offset += sizeof(be);
    *out = ntohs(be);
    return true;
}

bool read_be32(const std::vector<uint8_t>& frame, std::size_t* offset, uint32_t* out, std::string* error) {
    if (!take(sizeof(uint32_t), frame.size(), offset, error)) {
        return false;
    }
    uint32_t be = 0;
    std::memcpy(&be, frame.data() + *offset, sizeof(be));
    *offset += sizeof(be);
    *out = ntohl(be);
    return true;
}

bool read_be64(const std::vector<uint8_t>& frame, std::size_t* offset, uint64_t* out, std::string* error) {
    if (!take(sizeof(uint64_t), frame.size(), offset, error)) {
        return false;
    }
    uint32_t hi_be = 0;
    uint32_t lo_be = 0;
    std::memcpy(&hi_be, frame.data() + *offset, sizeof(hi_be));
    std::memcpy(&lo_be, frame.data() + *offset + sizeof(hi_be), sizeof(lo_be));
    *offset += sizeof(uint64_t);
    *out = (static_cast<uint64_t>(ntohl(hi_be)) << 32) | static_cast<uint64_t>(ntohl(lo_be));
    return true;
}

bool read_string(const std::vector<uint8_t>& frame,
                 std::size_t* offset,
                 std::string* out,
                 std::string* error,
                 const WasmLimits& limits) {
    uint16_t len = 0;
    if (!read_be16(frame, offset, &len, error)) {
        return false;
    }
    if (len > limits.max_string_bytes) {
        if (error != nullptr) {
            *error = "string length exceeds configured limit";
        }
        return false;
    }
    if (!take(len, frame.size(), offset, error)) {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(frame.data() + *offset), len);
    *offset += len;
    return true;
}

bool read_blob(const std::vector<uint8_t>& frame,
               std::size_t* offset,
               std::vector<uint8_t>* out,
               std::string* error,
               const WasmLimits& limits) {
    uint32_t len = 0;
    if (!read_be32(frame, offset, &len, error)) {
        return false;
    }
    if (len > limits.max_payload_bytes) {
        if (error != nullptr) {
            *error = "payload length exceeds configured limit";
        }
        return false;
    }
    if (!take(len, frame.size(), offset, error)) {
        return false;
    }
    out->assign(frame.begin() + static_cast<std::ptrdiff_t>(*offset),
                frame.begin() + static_cast<std::ptrdiff_t>(*offset + len));
    *offset += len;
    return true;
}

bool read_buffer_descriptors(const std::vector<uint8_t>& frame,
                             std::size_t* offset,
                             std::vector<WasmBufferDescriptor>* out,
                             std::string* error,
                             const WasmLimits& limits) {
    uint16_t count = 0;
    if (!read_be16(frame, offset, &count, error)) {
        return false;
    }
    if (count > limits.max_buffers) {
        if (error != nullptr) {
            *error = "buffer descriptor count exceeds configured limit";
        }
        return false;
    }
    out->clear();
    out->reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        WasmBufferDescriptor desc;
        if (!read_be32(frame, offset, &desc.buffer_id, error) ||
            !read_be32(frame, offset, &desc.flags, error) ||
            !read_be64(frame, offset, &desc.offset, error) ||
            !read_be64(frame, offset, &desc.length, error) ||
            !read_string(frame, offset, &desc.name, error, limits)) {
            return false;
        }
        out->push_back(std::move(desc));
    }
    return true;
}

bool decode_header(const std::vector<uint8_t>& frame,
                   WasmMessageType expected_type,
                   std::size_t* offset,
                   uint8_t* out_action_or_flags,
                   std::string* error,
                   const WasmLimits& limits) {
    if (frame.empty() || frame.size() > limits.max_frame_bytes) {
        if (error != nullptr) {
            *error = "invalid frame size";
        }
        return false;
    }

    // Header size: magic(4) + version(2) + msg_type(1) + action_or_flags(1) = 8 bytes
    if (!take(8, frame.size(), offset, error)) {
        return false;
    }

    uint32_t magic = 0;
    if (!read_be32(frame, offset, &magic, error)) {
        return false;
    }
    if (magic != WASM_MAGIC) {
        if (error != nullptr) {
            *error = "invalid WSM1 protocol magic";
        }
        return false;
    }

    uint16_t version = 0;
    if (!read_be16(frame, offset, &version, error)) {
        return false;
    }
    if (version != WASM_PROTOCOL_VERSION) {
        if (error != nullptr) {
            *error = "unsupported WSM1 protocol version";
        }
        return false;
    }

    const uint8_t msg_type_raw = frame[*offset];
    *offset += 1;
    if (msg_type_raw != static_cast<uint8_t>(expected_type)) {
        if (error != nullptr) {
            *error = "unexpected WSM1 message type";
        }
        return false;
    }

    if (out_action_or_flags != nullptr) {
        *out_action_or_flags = frame[*offset];
    }
    *offset += 1;
    return true;
}

void append_header(std::vector<uint8_t>* frame, WasmMessageType type, uint8_t action_or_flags) {
    append_be32(frame, WASM_MAGIC);
    append_be16(frame, WASM_PROTOCOL_VERSION);
    const uint8_t type_raw = static_cast<uint8_t>(type);
    append_bytes(frame, &type_raw, 1);
    append_bytes(frame, &action_or_flags, 1);
}

} // namespace

bool is_wasm_request_kind(const std::string& kind) noexcept {
    return kind == kWasmTaskKind ||
           kind == kWasmLifecycleKind ||
           kind == kWasmLifecyclePrepareKind ||
           kind == kWasmLifecycleDrainKind ||
           kind == kWasmLifecycleReleaseKind ||
           kind == kWasmLifecycleQueryKind;
}

const char* wasm_task_status_name(WasmTaskStatus status) noexcept {
    switch (status) {
        case WasmTaskStatus::Ok: return "Ok";
        case WasmTaskStatus::Rejected: return "Rejected";
        case WasmTaskStatus::Error: return "Error";
        case WasmTaskStatus::Timeout: return "Timeout";
        case WasmTaskStatus::ModuleDraining: return "ModuleDraining";
        case WasmTaskStatus::ModuleNotFound: return "ModuleNotFound";
        case WasmTaskStatus::VersionMismatch: return "VersionMismatch";
        default: return "Unknown";
    }
}

const char* wasm_lifecycle_action_name(WasmLifecycleAction action) noexcept {
    switch (action) {
        case WasmLifecycleAction::Prepare: return "Prepare";
        case WasmLifecycleAction::Drain: return "Drain";
        case WasmLifecycleAction::Release: return "Release";
        case WasmLifecycleAction::Query: return "Query";
        default: return "Unknown";
    }
}

const char* wasm_lifecycle_status_name(WasmLifecycleStatus status) noexcept {
    switch (status) {
        case WasmLifecycleStatus::Ok: return "Ok";
        case WasmLifecycleStatus::Rejected: return "Rejected";
        case WasmLifecycleStatus::Error: return "Error";
        case WasmLifecycleStatus::Busy: return "Busy";
        case WasmLifecycleStatus::NotFound: return "NotFound";
        default: return "Unknown";
    }
}

const char* wasm_module_state_name(WasmModuleState state) noexcept {
    switch (state) {
        case WasmModuleState::Unknown: return "Unknown";
        case WasmModuleState::Prepared: return "Prepared";
        case WasmModuleState::Active: return "Active";
        case WasmModuleState::Draining: return "Draining";
        case WasmModuleState::Stopped: return "Stopped";
        case WasmModuleState::Released: return "Released";
        default: return "Unknown";
    }
}

bool encode_wasm_task_request(const WasmTaskRequest& req,
                              std::vector<uint8_t>* out,
                              std::string* error,
                              const WasmLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) *error = "null output vector";
        return false;
    }
    if (req.task_id.empty() || req.sandbox_id.empty() || req.module_id.empty() ||
        req.module_version.empty() || req.task_kind.empty()) {
        if (error != nullptr) {
            *error = "task_id, sandbox_id, module_id, module_version, and task_kind must be non-empty";
        }
        return false;
    }

    std::vector<uint8_t> frame;
    frame.reserve(128 + req.payload.size());
    append_header(&frame, WasmMessageType::TaskRequest, 0);

    if (!append_string(&frame, req.task_id, limits, error) ||
        !append_string(&frame, req.sandbox_id, limits, error) ||
        !append_string(&frame, req.module_id, limits, error) ||
        !append_string(&frame, req.module_version, limits, error) ||
        !append_string(&frame, req.task_kind, limits, error)) {
        return false;
    }

    append_bytes(&frame, &req.priority, 1);
    append_be64(&frame, req.deadline_ms);

    if (!append_buffer_descriptors(&frame, req.buffer_descriptors, limits, error) ||
        !append_blob(&frame, req.payload, limits, error)) {
        return false;
    }

    if (frame.size() > limits.max_frame_bytes) {
        if (error != nullptr) {
            *error = "encoded task request exceeds max_frame_bytes";
        }
        return false;
    }

    *out = std::move(frame);
    return true;
}

bool decode_wasm_task_request(const std::vector<uint8_t>& frame,
                              WasmTaskRequest* out,
                              std::string* error,
                              const WasmLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) *error = "null output pointer";
        return false;
    }

    std::size_t offset = 0;
    uint8_t flags = 0;
    if (!decode_header(frame, WasmMessageType::TaskRequest, &offset, &flags, error, limits)) {
        return false;
    }

    WasmTaskRequest req;
    if (!read_string(frame, &offset, &req.task_id, error, limits) ||
        !read_string(frame, &offset, &req.sandbox_id, error, limits) ||
        !read_string(frame, &offset, &req.module_id, error, limits) ||
        !read_string(frame, &offset, &req.module_version, error, limits) ||
        !read_string(frame, &offset, &req.task_kind, error, limits)) {
        return false;
    }

    if (!take(1, frame.size(), &offset, error)) {
        return false;
    }
    req.priority = frame[offset++];

    if (!read_be64(frame, &offset, &req.deadline_ms, error) ||
        !read_buffer_descriptors(frame, &offset, &req.buffer_descriptors, error, limits) ||
        !read_blob(frame, &offset, &req.payload, error, limits)) {
        return false;
    }

    if (offset != frame.size()) {
        if (error != nullptr) {
            *error = "task request frame contains trailing bytes";
        }
        return false;
    }

    if (req.task_id.empty() || req.sandbox_id.empty() || req.module_id.empty() ||
        req.module_version.empty() || req.task_kind.empty()) {
        if (error != nullptr) {
            *error = "task_id, sandbox_id, module_id, module_version, and task_kind must be non-empty";
        }
        return false;
    }

    *out = std::move(req);
    return true;
}

bool encode_wasm_task_response(const WasmTaskResponse& resp,
                               std::vector<uint8_t>* out,
                               std::string* error,
                               const WasmLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) *error = "null output vector";
        return false;
    }

    std::vector<uint8_t> frame;
    frame.reserve(128 + resp.result.size());
    const uint8_t status_raw = static_cast<uint8_t>(resp.status);
    append_header(&frame, WasmMessageType::TaskResponse, status_raw);

    append_be16(&frame, resp.protocol_version);
    if (!append_string(&frame, resp.task_id, limits, error)) {
        return false;
    }

    append_be32(&frame, static_cast<uint32_t>(resp.selected_gpu));
    append_be64(&frame, resp.latency_ms);

    if (!append_string(&frame, resp.error, limits, error) ||
        !append_buffer_descriptors(&frame, resp.buffer_descriptors, limits, error) ||
        !append_blob(&frame, resp.result, limits, error)) {
        return false;
    }

    if (frame.size() > limits.max_frame_bytes) {
        if (error != nullptr) {
            *error = "encoded task response exceeds max_frame_bytes";
        }
        return false;
    }

    *out = std::move(frame);
    return true;
}

bool decode_wasm_task_response(const std::vector<uint8_t>& frame,
                               WasmTaskResponse* out,
                               std::string* error,
                               const WasmLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) *error = "null output pointer";
        return false;
    }

    std::size_t offset = 0;
    uint8_t status_raw = 0;
    if (!decode_header(frame, WasmMessageType::TaskResponse, &offset, &status_raw, error, limits)) {
        return false;
    }

    WasmTaskResponse resp;
    resp.status = static_cast<WasmTaskStatus>(status_raw);

    if (!read_be16(frame, &offset, &resp.protocol_version, error) ||
        !read_string(frame, &offset, &resp.task_id, error, limits)) {
        return false;
    }

    uint32_t gpu_be = 0;
    if (!read_be32(frame, &offset, &gpu_be, error)) {
        return false;
    }
    resp.selected_gpu = static_cast<int32_t>(gpu_be);

    if (!read_be64(frame, &offset, &resp.latency_ms, error) ||
        !read_string(frame, &offset, &resp.error, error, limits) ||
        !read_buffer_descriptors(frame, &offset, &resp.buffer_descriptors, error, limits) ||
        !read_blob(frame, &offset, &resp.result, error, limits)) {
        return false;
    }

    if (offset != frame.size()) {
        if (error != nullptr) {
            *error = "task response frame contains trailing bytes";
        }
        return false;
    }

    *out = std::move(resp);
    return true;
}

bool encode_wasm_lifecycle_request(const WasmLifecycleRequest& req,
                                   std::vector<uint8_t>* out,
                                   std::string* error,
                                   const WasmLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) *error = "null output vector";
        return false;
    }
    if (req.request_id.empty() || req.sandbox_id.empty() || req.module_id.empty() || req.module_version.empty()) {
        if (error != nullptr) {
            *error = "request_id, sandbox_id, module_id, and module_version must be non-empty";
        }
        return false;
    }

    std::vector<uint8_t> frame;
    frame.reserve(128 + req.payload.size());
    const uint8_t action_raw = static_cast<uint8_t>(req.action);
    append_header(&frame, WasmMessageType::LifecycleRequest, action_raw);

    if (!append_string(&frame, req.request_id, limits, error) ||
        !append_string(&frame, req.sandbox_id, limits, error) ||
        !append_string(&frame, req.module_id, limits, error) ||
        !append_string(&frame, req.module_version, limits, error)) {
        return false;
    }

    append_be32(&frame, static_cast<uint32_t>(req.target_gpu));
    append_be32(&frame, req.grace_period_ms);

    if (!append_string(&frame, req.ack_token, limits, error) ||
        !append_blob(&frame, req.payload, limits, error)) {
        return false;
    }

    if (frame.size() > limits.max_frame_bytes) {
        if (error != nullptr) {
            *error = "encoded lifecycle request exceeds max_frame_bytes";
        }
        return false;
    }

    *out = std::move(frame);
    return true;
}

bool decode_wasm_lifecycle_request(const std::vector<uint8_t>& frame,
                                   WasmLifecycleRequest* out,
                                   std::string* error,
                                   const WasmLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) *error = "null output pointer";
        return false;
    }

    std::size_t offset = 0;
    uint8_t action_raw = 0;
    if (!decode_header(frame, WasmMessageType::LifecycleRequest, &offset, &action_raw, error, limits)) {
        return false;
    }

    WasmLifecycleRequest req;
    req.action = static_cast<WasmLifecycleAction>(action_raw);

    if (!read_string(frame, &offset, &req.request_id, error, limits) ||
        !read_string(frame, &offset, &req.sandbox_id, error, limits) ||
        !read_string(frame, &offset, &req.module_id, error, limits) ||
        !read_string(frame, &offset, &req.module_version, error, limits)) {
        return false;
    }

    uint32_t target_gpu_be = 0;
    if (!read_be32(frame, &offset, &target_gpu_be, error)) {
        return false;
    }
    req.target_gpu = static_cast<int32_t>(target_gpu_be);

    if (!read_be32(frame, &offset, &req.grace_period_ms, error) ||
        !read_string(frame, &offset, &req.ack_token, error, limits) ||
        !read_blob(frame, &offset, &req.payload, error, limits)) {
        return false;
    }

    if (offset != frame.size()) {
        if (error != nullptr) {
            *error = "lifecycle request frame contains trailing bytes";
        }
        return false;
    }

    if (req.request_id.empty() || req.sandbox_id.empty() || req.module_id.empty() || req.module_version.empty()) {
        if (error != nullptr) {
            *error = "request_id, sandbox_id, module_id, and module_version must be non-empty";
        }
        return false;
    }

    *out = std::move(req);
    return true;
}

bool encode_wasm_lifecycle_response(const WasmLifecycleResponse& resp,
                                    std::vector<uint8_t>* out,
                                    std::string* error,
                                    const WasmLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) *error = "null output vector";
        return false;
    }

    std::vector<uint8_t> frame;
    frame.reserve(128);
    const uint8_t action_raw = static_cast<uint8_t>(resp.action);
    append_header(&frame, WasmMessageType::LifecycleResponse, action_raw);

    const uint8_t status_raw = static_cast<uint8_t>(resp.status);
    append_bytes(&frame, &status_raw, 1);
    append_be16(&frame, resp.protocol_version);

    const uint8_t state_raw = static_cast<uint8_t>(resp.state);
    append_bytes(&frame, &state_raw, 1);

    if (!append_string(&frame, resp.request_id, limits, error) ||
        !append_string(&frame, resp.sandbox_id, limits, error) ||
        !append_string(&frame, resp.module_id, limits, error) ||
        !append_string(&frame, resp.module_version, limits, error)) {
        return false;
    }

    append_be32(&frame, static_cast<uint32_t>(resp.assigned_gpu));
    append_be32(&frame, resp.active_tasks);

    if (!append_string(&frame, resp.lease_token, limits, error) ||
        !append_string(&frame, resp.error, limits, error)) {
        return false;
    }

    if (frame.size() > limits.max_frame_bytes) {
        if (error != nullptr) {
            *error = "encoded lifecycle response exceeds max_frame_bytes";
        }
        return false;
    }

    *out = std::move(frame);
    return true;
}

bool decode_wasm_lifecycle_response(const std::vector<uint8_t>& frame,
                                    WasmLifecycleResponse* out,
                                    std::string* error,
                                    const WasmLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) *error = "null output pointer";
        return false;
    }

    std::size_t offset = 0;
    uint8_t action_raw = 0;
    if (!decode_header(frame, WasmMessageType::LifecycleResponse, &offset, &action_raw, error, limits)) {
        return false;
    }

    WasmLifecycleResponse resp;
    resp.action = static_cast<WasmLifecycleAction>(action_raw);

    if (!take(1, frame.size(), &offset, error)) {
        return false;
    }
    resp.status = static_cast<WasmLifecycleStatus>(frame[offset++]);

    if (!read_be16(frame, &offset, &resp.protocol_version, error)) {
        return false;
    }

    if (!take(1, frame.size(), &offset, error)) {
        return false;
    }
    resp.state = static_cast<WasmModuleState>(frame[offset++]);

    if (!read_string(frame, &offset, &resp.request_id, error, limits) ||
        !read_string(frame, &offset, &resp.sandbox_id, error, limits) ||
        !read_string(frame, &offset, &resp.module_id, error, limits) ||
        !read_string(frame, &offset, &resp.module_version, error, limits)) {
        return false;
    }

    uint32_t assigned_gpu_be = 0;
    if (!read_be32(frame, &offset, &assigned_gpu_be, error)) {
        return false;
    }
    resp.assigned_gpu = static_cast<int32_t>(assigned_gpu_be);

    if (!read_be32(frame, &offset, &resp.active_tasks, error) ||
        !read_string(frame, &offset, &resp.lease_token, error, limits) ||
        !read_string(frame, &offset, &resp.error, error, limits)) {
        return false;
    }

    if (offset != frame.size()) {
        if (error != nullptr) {
            *error = "lifecycle response frame contains trailing bytes";
        }
        return false;
    }

    *out = std::move(resp);
    return true;
}

// -----------------------------------------------------------------------------
// WasmSandboxManager Implementation
// -----------------------------------------------------------------------------

WasmSandboxManager::WasmSandboxManager(bool force_cpu, GpuSelectorFn gpu_selector)
    : force_cpu_(force_cpu), gpu_selector_(std::move(gpu_selector)) {}

void WasmSandboxManager::set_force_cpu(bool force_cpu) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    force_cpu_ = force_cpu;
}

bool WasmSandboxManager::force_cpu() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return force_cpu_;
}

void WasmSandboxManager::set_gpu_selector(GpuSelectorFn gpu_selector) {
    std::lock_guard<std::mutex> lock(mutex_);
    gpu_selector_ = std::move(gpu_selector);
}

std::string WasmSandboxManager::make_key(const std::string& sandbox_id,
                                        const std::string& module_id,
                                        const std::string& version) {
    return sandbox_id + ":" + module_id + ":" + version;
}

WasmLifecycleResponse WasmSandboxManager::handle_lifecycle(const WasmLifecycleRequest& request) {
    switch (request.action) {
        case WasmLifecycleAction::Prepare:
            return prepare_module(request);
        case WasmLifecycleAction::Drain:
            return drain_module(request);
        case WasmLifecycleAction::Release:
            return release_module(request);
        case WasmLifecycleAction::Query:
            return query_module(request);
        default: {
            WasmLifecycleResponse resp;
            resp.action = request.action;
            resp.status = WasmLifecycleStatus::Error;
            resp.request_id = request.request_id;
            resp.sandbox_id = request.sandbox_id;
            resp.module_id = request.module_id;
            resp.module_version = request.module_version;
            resp.error = "unknown lifecycle action";
            return resp;
        }
    }
}

WasmLifecycleResponse WasmSandboxManager::prepare_module(const WasmLifecycleRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    WasmLifecycleResponse resp;
    resp.action = WasmLifecycleAction::Prepare;
    resp.request_id = request.request_id;
    resp.sandbox_id = request.sandbox_id;
    resp.module_id = request.module_id;
    resp.module_version = request.module_version;
    resp.protocol_version = WASM_PROTOCOL_VERSION;

    if (request.sandbox_id.empty() || request.module_id.empty() || request.module_version.empty()) {
        resp.status = WasmLifecycleStatus::Rejected;
        resp.error = "sandbox_id, module_id, and module_version must be non-empty";
        return resp;
    }

    const std::string key = make_key(request.sandbox_id, request.module_id, request.module_version);
    auto it = modules_.find(key);
    if (it != modules_.end()) {
        if (it->second.state == WasmModuleState::Prepared || it->second.state == WasmModuleState::Active) {
            // Idempotent re-registration of existing active/prepared module
            resp.status = WasmLifecycleStatus::Ok;
            resp.state = it->second.state;
            resp.assigned_gpu = it->second.assigned_gpu;
            resp.active_tasks = it->second.active_tasks;
            resp.lease_token = it->second.lease_token;
            return resp;
        }
        if (it->second.state == WasmModuleState::Draining || it->second.state == WasmModuleState::Stopped) {
            resp.status = WasmLifecycleStatus::Busy;
            resp.state = it->second.state;
            resp.error = "module version is currently draining or stopped; cannot prepare until released";
            return resp;
        }
    }

    int32_t assigned_gpu = -1;
    if (request.target_gpu >= 0) {
        assigned_gpu = request.target_gpu;
    } else if (force_cpu_) {
        assigned_gpu = -1;
    } else if (gpu_selector_) {
        assigned_gpu = gpu_selector_(request.sandbox_id, request.target_gpu);
    }

    const std::string lease = "lease-" + request.sandbox_id + "-" + request.module_id +
                              "-" + request.module_version + "-" + std::to_string(next_lease_id_++);

    ModuleRecord rec;
    rec.sandbox_id = request.sandbox_id;
    rec.module_id = request.module_id;
    rec.version = request.module_version;
    rec.state = WasmModuleState::Prepared;
    rec.assigned_gpu = assigned_gpu;
    rec.lease_token = lease;
    rec.active_tasks = 0;

    modules_[key] = rec;

    resp.status = WasmLifecycleStatus::Ok;
    resp.state = WasmModuleState::Prepared;
    resp.assigned_gpu = assigned_gpu;
    resp.active_tasks = 0;
    resp.lease_token = lease;
    return resp;
}

WasmLifecycleResponse WasmSandboxManager::drain_module(const WasmLifecycleRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    WasmLifecycleResponse resp;
    resp.action = WasmLifecycleAction::Drain;
    resp.request_id = request.request_id;
    resp.sandbox_id = request.sandbox_id;
    resp.module_id = request.module_id;
    resp.module_version = request.module_version;
    resp.protocol_version = WASM_PROTOCOL_VERSION;

    const std::string key = make_key(request.sandbox_id, request.module_id, request.module_version);
    auto it = modules_.find(key);
    if (it == modules_.end()) {
        resp.status = WasmLifecycleStatus::NotFound;
        resp.error = "module version not registered";
        return resp;
    }

    auto& rec = it->second;
    if (rec.state == WasmModuleState::Released) {
        resp.status = WasmLifecycleStatus::NotFound;
        resp.error = "module version already released";
        return resp;
    }

    rec.state = (rec.active_tasks == 0) ? WasmModuleState::Stopped : WasmModuleState::Draining;

    resp.status = WasmLifecycleStatus::Ok;
    resp.state = rec.state;
    resp.assigned_gpu = rec.assigned_gpu;
    resp.active_tasks = rec.active_tasks;
    resp.lease_token = rec.lease_token;
    return resp;
}

WasmLifecycleResponse WasmSandboxManager::release_module(const WasmLifecycleRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    WasmLifecycleResponse resp;
    resp.action = WasmLifecycleAction::Release;
    resp.request_id = request.request_id;
    resp.sandbox_id = request.sandbox_id;
    resp.module_id = request.module_id;
    resp.module_version = request.module_version;
    resp.protocol_version = WASM_PROTOCOL_VERSION;

    const std::string key = make_key(request.sandbox_id, request.module_id, request.module_version);
    auto it = modules_.find(key);
    if (it == modules_.end()) {
        resp.status = WasmLifecycleStatus::NotFound;
        resp.error = "module version not registered";
        return resp;
    }

    auto& rec = it->second;
    if (rec.active_tasks > 0) {
        resp.status = WasmLifecycleStatus::Busy;
        resp.state = rec.state;
        resp.active_tasks = rec.active_tasks;
        resp.assigned_gpu = rec.assigned_gpu;
        resp.lease_token = rec.lease_token;
        resp.error = "cannot release module version while tasks are in-flight; wait for drain to complete";
        return resp;
    }

    rec.state = WasmModuleState::Released;
    resp.status = WasmLifecycleStatus::Ok;
    resp.state = WasmModuleState::Released;
    resp.assigned_gpu = rec.assigned_gpu;
    resp.active_tasks = 0;
    resp.lease_token = rec.lease_token;

    modules_.erase(it);
    return resp;
}

WasmLifecycleResponse WasmSandboxManager::query_module(const WasmLifecycleRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    WasmLifecycleResponse resp;
    resp.action = WasmLifecycleAction::Query;
    resp.request_id = request.request_id;
    resp.sandbox_id = request.sandbox_id;
    resp.module_id = request.module_id;
    resp.module_version = request.module_version;
    resp.protocol_version = WASM_PROTOCOL_VERSION;

    const std::string key = make_key(request.sandbox_id, request.module_id, request.module_version);
    auto it = modules_.find(key);
    if (it == modules_.end()) {
        resp.status = WasmLifecycleStatus::NotFound;
        resp.state = WasmModuleState::Unknown;
        resp.error = "module version not registered";
        return resp;
    }

    const auto& rec = it->second;
    resp.status = WasmLifecycleStatus::Ok;
    resp.state = rec.state;
    resp.assigned_gpu = rec.assigned_gpu;
    resp.active_tasks = rec.active_tasks;
    resp.lease_token = rec.lease_token;
    return resp;
}

bool WasmSandboxManager::is_module_active(const std::string& sandbox_id,
                                          const std::string& module_id,
                                          const std::string& version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = modules_.find(make_key(sandbox_id, module_id, version));
    if (it == modules_.end()) return false;
    return it->second.state == WasmModuleState::Prepared || it->second.state == WasmModuleState::Active;
}

WasmModuleState WasmSandboxManager::get_module_state(const std::string& sandbox_id,
                                                    const std::string& module_id,
                                                    const std::string& version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = modules_.find(make_key(sandbox_id, module_id, version));
    if (it == modules_.end()) return WasmModuleState::Unknown;
    return it->second.state;
}

uint32_t WasmSandboxManager::get_active_tasks(const std::string& sandbox_id,
                                              const std::string& module_id,
                                              const std::string& version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = modules_.find(make_key(sandbox_id, module_id, version));
    if (it == modules_.end()) return 0;
    return it->second.active_tasks;
}

WasmTaskResponse WasmSandboxManager::handle_task(const WasmTaskRequest& request, ComputeFn compute_fn) {
    WasmTaskResponse resp;
    resp.task_id = request.task_id;
    resp.protocol_version = WASM_PROTOCOL_VERSION;
    resp.buffer_descriptors = request.buffer_descriptors;

    int32_t assigned_gpu = -1;
    const std::string key = make_key(request.sandbox_id, request.module_id, request.module_version);

    // Module check and inflight increment under lock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = modules_.find(key);
        if (it == modules_.end()) {
            resp.status = WasmTaskStatus::ModuleNotFound;
            resp.error = "module version " + request.module_version + " is not registered";
            return resp;
        }

        auto& rec = it->second;
        if (rec.state == WasmModuleState::Draining) {
            resp.status = WasmTaskStatus::ModuleDraining;
            resp.error = "module version " + request.module_version + " is draining; hotswap in progress";
            return resp;
        }
        if (rec.state == WasmModuleState::Stopped || rec.state == WasmModuleState::Released) {
            resp.status = WasmTaskStatus::Rejected;
            resp.error = "module version " + request.module_version + " is stopped or released";
            return resp;
        }

        rec.state = WasmModuleState::Active;
        ++rec.active_tasks;
        assigned_gpu = rec.assigned_gpu;
    }

    const auto start_time = std::chrono::steady_clock::now();
    resp.selected_gpu = assigned_gpu;

    // Execute compute outside module mutex
    try {
        if (compute_fn != nullptr) {
            resp.result = compute_fn(assigned_gpu, request);
        } else if (!request.payload.empty()) {
            if (request.payload.size() % sizeof(double) == 0) {
                // If payload decodes as packed array of doubles, use standard ADI GPU/CPU compute
                std::vector<double> doubles(request.payload.size() / sizeof(double));
                std::memcpy(doubles.data(), request.payload.data(), request.payload.size());
                const int compute_gpu = (assigned_gpu >= 0) ? assigned_gpu : 0;
                const std::vector<double> out_doubles = nvlink::adi::default_gpu_compute(doubles, compute_gpu);
                resp.result.resize(out_doubles.size() * sizeof(double));
                std::memcpy(resp.result.data(), out_doubles.data(), resp.result.size());
            } else {
                // Echo payload transformed
                resp.result = request.payload;
            }
        }
        resp.status = WasmTaskStatus::Ok;
    } catch (const std::exception& ex) {
        resp.status = WasmTaskStatus::Error;
        resp.error = ex.what();
    }

    const auto end_time = std::chrono::steady_clock::now();
    resp.latency_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

    // Decrement inflight tasks and auto-transition to Stopped if draining
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = modules_.find(key);
        if (it != modules_.end()) {
            auto& rec = it->second;
            if (rec.active_tasks > 0) {
                --rec.active_tasks;
            }
            if (rec.state == WasmModuleState::Draining && rec.active_tasks == 0) {
                rec.state = WasmModuleState::Stopped;
            }
        }
    }

    return resp;
}

} // namespace nvlink::wasm
