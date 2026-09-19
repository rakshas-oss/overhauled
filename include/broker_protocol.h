#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nvlink::broker {

constexpr uint32_t BROKER_MAGIC = 0x42524b31U; // BRK1
constexpr uint16_t BROKER_PROTOCOL_VERSION = 1;

enum class MessageType : uint8_t {
    Request = 1,
    Response = 2,
};

enum class TaskStatus : uint8_t {
    Ok = 0,
    Rejected = 1,
    Error = 2,
    Timeout = 3,
};

struct ProtocolLimits {
    std::size_t max_frame_bytes = 1024 * 1024;
    std::size_t max_payload_bytes = 512 * 1024;
    std::size_t max_string_bytes = 1024;
};

struct BrokerRequest {
    std::string task_id;
    std::string source;
    std::string destination;
    std::string kind;
    uint8_t priority = 0;
    uint32_t timeout_ms = 0;
    std::vector<uint8_t> payload;
};

struct BrokerResponse {
    std::string task_id;
    TaskStatus status = TaskStatus::Error;
    int32_t selected_gpu = -1;
    uint64_t latency_ms = 0;
    std::vector<uint8_t> result;
    std::string error;
};

bool validate_request(const BrokerRequest& request, std::string* error, const ProtocolLimits& limits);

std::vector<uint8_t> encode_request_frame(const BrokerRequest& request, const ProtocolLimits& limits);
std::vector<uint8_t> encode_response_frame(const BrokerResponse& response, const ProtocolLimits& limits);

bool decode_request_frame(const std::vector<uint8_t>& frame,
                          BrokerRequest* out,
                          std::string* error,
                          const ProtocolLimits& limits);

bool decode_response_frame(const std::vector<uint8_t>& frame,
                           BrokerResponse* out,
                           std::string* error,
                           const ProtocolLimits& limits);

const char* status_name(TaskStatus status) noexcept;

} // namespace nvlink::broker
