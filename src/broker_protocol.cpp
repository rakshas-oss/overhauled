#include "broker_protocol.h"

#include <arpa/inet.h>

#include <cstring>
#include <limits>
#include <stdexcept>

namespace nvlink::broker {
namespace {

uint64_t host_to_be64(uint64_t value) noexcept {
    const uint64_t high = static_cast<uint64_t>(htonl(static_cast<uint32_t>(value >> 32)));
    const uint64_t low = static_cast<uint64_t>(htonl(static_cast<uint32_t>(value & 0xffffffffULL)));
    return (low << 32) | high;
}

uint64_t be64_to_host(uint64_t value) noexcept {
    const uint64_t high = static_cast<uint64_t>(ntohl(static_cast<uint32_t>(value >> 32)));
    const uint64_t low = static_cast<uint64_t>(ntohl(static_cast<uint32_t>(value & 0xffffffffULL)));
    return (low << 32) | high;
}

void append_bytes(std::vector<uint8_t>* out, const void* data, std::size_t size) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    out->insert(out->end(), ptr, ptr + size);
}

bool append_string(std::vector<uint8_t>* out,
                   const std::string& value,
                   const ProtocolLimits& limits,
                   std::string* error) {
    if (value.size() > limits.max_string_bytes || value.size() > std::numeric_limits<uint16_t>::max()) {
        if (error != nullptr) {
            *error = "string exceeds maximum length";
        }
        return false;
    }
    const uint16_t len_be = htons(static_cast<uint16_t>(value.size()));
    append_bytes(out, &len_be, sizeof(len_be));
    append_bytes(out, value.data(), value.size());
    return true;
}

bool append_blob(std::vector<uint8_t>* out,
                 const std::vector<uint8_t>& value,
                 const ProtocolLimits& limits,
                 std::string* error) {
    if (value.size() > limits.max_payload_bytes || value.size() > std::numeric_limits<uint32_t>::max()) {
        if (error != nullptr) {
            *error = "payload exceeds maximum length";
        }
        return false;
    }
    const uint32_t len_be = htonl(static_cast<uint32_t>(value.size()));
    append_bytes(out, &len_be, sizeof(len_be));
    append_bytes(out, value.data(), value.size());
    return true;
}

bool take(std::size_t need, std::size_t size, std::size_t* offset, std::string* error) {
    if (*offset > size || need > size - *offset) {
        if (error != nullptr) {
            *error = "frame truncated";
        }
        return false;
    }
    return true;
}

bool read_string(const std::vector<uint8_t>& frame,
                 std::size_t* offset,
                 std::string* out,
                 std::string* error,
                 const ProtocolLimits& limits) {
    if (!take(sizeof(uint16_t), frame.size(), offset, error)) {
        return false;
    }
    uint16_t len_be = 0;
    std::memcpy(&len_be, frame.data() + *offset, sizeof(len_be));
    *offset += sizeof(len_be);
    const std::size_t len = ntohs(len_be);
    if (len > limits.max_string_bytes) {
        if (error != nullptr) {
            *error = "string exceeds configured limit";
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
               const ProtocolLimits& limits) {
    if (!take(sizeof(uint32_t), frame.size(), offset, error)) {
        return false;
    }
    uint32_t len_be = 0;
    std::memcpy(&len_be, frame.data() + *offset, sizeof(len_be));
    *offset += sizeof(len_be);
    const std::size_t len = ntohl(len_be);
    if (len > limits.max_payload_bytes) {
        if (error != nullptr) {
            *error = "payload exceeds configured limit";
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

bool decode_common_header(const std::vector<uint8_t>& frame,
                          MessageType expected_type,
                          std::size_t* offset,
                          std::string* error,
                          const ProtocolLimits& limits) {
    if (frame.empty() || frame.size() > limits.max_frame_bytes) {
        if (error != nullptr) {
            *error = "invalid frame size";
        }
        return false;
    }

    if (!take(sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint8_t),
              frame.size(),
              offset,
              error)) {
        return false;
    }

    uint32_t magic_be = 0;
    uint16_t version_be = 0;
    uint8_t type_raw = 0;
    std::memcpy(&magic_be, frame.data(), sizeof(magic_be));
    std::memcpy(&version_be, frame.data() + sizeof(magic_be), sizeof(version_be));
    std::memcpy(&type_raw, frame.data() + sizeof(magic_be) + sizeof(version_be), sizeof(type_raw));

    *offset = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint8_t);

    if (ntohl(magic_be) != BROKER_MAGIC) {
        if (error != nullptr) {
            *error = "invalid broker protocol magic";
        }
        return false;
    }
    if (ntohs(version_be) != BROKER_PROTOCOL_VERSION) {
        if (error != nullptr) {
            *error = "unsupported broker protocol version";
        }
        return false;
    }
    if (type_raw != static_cast<uint8_t>(expected_type)) {
        if (error != nullptr) {
            *error = "unexpected broker protocol message type";
        }
        return false;
    }
    return true;
}

void append_common_header(std::vector<uint8_t>* frame, MessageType type) {
    const uint32_t magic_be = htonl(BROKER_MAGIC);
    const uint16_t version_be = htons(BROKER_PROTOCOL_VERSION);
    const uint8_t type_raw = static_cast<uint8_t>(type);
    const uint8_t reserved = 0;

    append_bytes(frame, &magic_be, sizeof(magic_be));
    append_bytes(frame, &version_be, sizeof(version_be));
    append_bytes(frame, &type_raw, sizeof(type_raw));
    append_bytes(frame, &reserved, sizeof(reserved));
}

} // namespace

bool validate_request(const BrokerRequest& request, std::string* error, const ProtocolLimits& limits) {
    if (request.task_id.empty()) {
        if (error != nullptr) {
            *error = "task_id must be non-empty";
        }
        return false;
    }
    if (request.source.empty() || request.destination.empty() || request.kind.empty()) {
        if (error != nullptr) {
            *error = "source, destination, and kind must be non-empty";
        }
        return false;
    }
    if (request.timeout_ms == 0) {
        if (error != nullptr) {
            *error = "timeout_ms must be greater than zero";
        }
        return false;
    }
    if (request.payload.size() > limits.max_payload_bytes) {
        if (error != nullptr) {
            *error = "payload exceeds max_payload_bytes";
        }
        return false;
    }
    return true;
}

std::vector<uint8_t> encode_request_frame(const BrokerRequest& request, const ProtocolLimits& limits) {
    std::string error;
    if (!validate_request(request, &error, limits)) {
        throw std::runtime_error(error);
    }

    std::vector<uint8_t> frame;
    frame.reserve(128 + request.payload.size());
    append_common_header(&frame, MessageType::Request);

    if (!append_string(&frame, request.task_id, limits, &error) ||
        !append_string(&frame, request.source, limits, &error) ||
        !append_string(&frame, request.destination, limits, &error) ||
        !append_string(&frame, request.kind, limits, &error)) {
        throw std::runtime_error(error);
    }

    append_bytes(&frame, &request.priority, sizeof(request.priority));
    const uint32_t timeout_be = htonl(request.timeout_ms);
    append_bytes(&frame, &timeout_be, sizeof(timeout_be));

    if (!append_blob(&frame, request.payload, limits, &error)) {
        throw std::runtime_error(error);
    }

    if (frame.size() > limits.max_frame_bytes) {
        throw std::runtime_error("encoded request frame exceeds max_frame_bytes");
    }
    return frame;
}

std::vector<uint8_t> encode_response_frame(const BrokerResponse& response, const ProtocolLimits& limits) {
    std::vector<uint8_t> frame;
    frame.reserve(128 + response.result.size());
    append_common_header(&frame, MessageType::Response);

    std::string error;
    if (!append_string(&frame, response.task_id, limits, &error)) {
        throw std::runtime_error(error);
    }

    const uint8_t status = static_cast<uint8_t>(response.status);
    append_bytes(&frame, &status, sizeof(status));

    const uint32_t gpu_be = htonl(static_cast<uint32_t>(response.selected_gpu));
    append_bytes(&frame, &gpu_be, sizeof(gpu_be));

    const uint64_t latency_be = host_to_be64(response.latency_ms);
    append_bytes(&frame, &latency_be, sizeof(latency_be));

    if (!append_blob(&frame, response.result, limits, &error) ||
        !append_string(&frame, response.error, limits, &error)) {
        throw std::runtime_error(error);
    }

    if (frame.size() > limits.max_frame_bytes) {
        throw std::runtime_error("encoded response frame exceeds max_frame_bytes");
    }
    return frame;
}

bool decode_request_frame(const std::vector<uint8_t>& frame,
                          BrokerRequest* out,
                          std::string* error,
                          const ProtocolLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "output request pointer must not be null";
        }
        return false;
    }

    std::size_t offset = 0;
    if (!decode_common_header(frame, MessageType::Request, &offset, error, limits)) {
        return false;
    }

    BrokerRequest request;
    if (!read_string(frame, &offset, &request.task_id, error, limits) ||
        !read_string(frame, &offset, &request.source, error, limits) ||
        !read_string(frame, &offset, &request.destination, error, limits) ||
        !read_string(frame, &offset, &request.kind, error, limits)) {
        return false;
    }

    if (!take(sizeof(uint8_t) + sizeof(uint32_t), frame.size(), &offset, error)) {
        return false;
    }
    std::memcpy(&request.priority, frame.data() + offset, sizeof(request.priority));
    offset += sizeof(request.priority);
    uint32_t timeout_be = 0;
    std::memcpy(&timeout_be, frame.data() + offset, sizeof(timeout_be));
    offset += sizeof(timeout_be);
    request.timeout_ms = ntohl(timeout_be);

    if (!read_blob(frame, &offset, &request.payload, error, limits)) {
        return false;
    }

    if (offset != frame.size()) {
        if (error != nullptr) {
            *error = "request frame contains trailing bytes";
        }
        return false;
    }

    if (!validate_request(request, error, limits)) {
        return false;
    }

    *out = std::move(request);
    return true;
}

bool decode_response_frame(const std::vector<uint8_t>& frame,
                           BrokerResponse* out,
                           std::string* error,
                           const ProtocolLimits& limits) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "output response pointer must not be null";
        }
        return false;
    }

    std::size_t offset = 0;
    if (!decode_common_header(frame, MessageType::Response, &offset, error, limits)) {
        return false;
    }

    BrokerResponse response;
    if (!read_string(frame, &offset, &response.task_id, error, limits)) {
        return false;
    }

    if (!take(sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint64_t), frame.size(), &offset, error)) {
        return false;
    }
    uint8_t status_raw = 0;
    std::memcpy(&status_raw, frame.data() + offset, sizeof(status_raw));
    offset += sizeof(status_raw);
    response.status = static_cast<TaskStatus>(status_raw);

    uint32_t gpu_be = 0;
    std::memcpy(&gpu_be, frame.data() + offset, sizeof(gpu_be));
    offset += sizeof(gpu_be);
    response.selected_gpu = static_cast<int32_t>(ntohl(gpu_be));

    uint64_t latency_be = 0;
    std::memcpy(&latency_be, frame.data() + offset, sizeof(latency_be));
    offset += sizeof(latency_be);
    response.latency_ms = be64_to_host(latency_be);

    if (!read_blob(frame, &offset, &response.result, error, limits) ||
        !read_string(frame, &offset, &response.error, error, limits)) {
        return false;
    }

    if (offset != frame.size()) {
        if (error != nullptr) {
            *error = "response frame contains trailing bytes";
        }
        return false;
    }

    *out = std::move(response);
    return true;
}

const char* status_name(TaskStatus status) noexcept {
    switch (status) {
        case TaskStatus::Ok:
            return "ok";
        case TaskStatus::Rejected:
            return "rejected";
        case TaskStatus::Error:
            return "error";
        case TaskStatus::Timeout:
            return "timeout";
        default:
            return "unknown";
    }
}

} // namespace nvlink::broker
