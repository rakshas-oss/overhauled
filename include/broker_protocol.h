// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors

#pragma once

#include "backend_dispatch.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nvlink::broker {

constexpr uint32_t BROKER_MAGIC = 0x42524b31U; // BRK1
constexpr uint16_t BROKER_PROTOCOL_VERSION = 1;
constexpr uint32_t BROKER_MAX_TIMEOUT_MS = 300000;

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

// The worker class is supplied by YuKKi-OS as workload intent. Overhauled
// validates it and applies local admission/placement without reclassifying the
// workload. BRK1 and BRK2 requests without the optional directive default to
// Throughput for compatibility.
enum class WorkerClass : uint8_t {
    Control = 0,
    RealtimeSpatial = 1,
    Throughput = 2,
    ExactCpu = 3,
    Inference = 4,
};

const char* worker_class_name(WorkerClass value) noexcept;
bool parse_worker_class(const std::string& value, WorkerClass* out) noexcept;

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
    // In-process only (not part of the BRK1 frame); absent means CPU by default.
    std::optional<TaskProfile> profile;
    // Optional wire-carried device preference, from BRK2 field 10 when present.
    // Values: "" (unset), "cpu", "gpu", "npu", "auto". Old servers ignore it;
    // new servers map "npu" to an inference profile (see BrokerService).
    std::string device_preference;
    WorkerClass worker_class = WorkerClass::Throughput;
    bool worker_class_explicit = false;
    std::optional<uint64_t> spatial_affinity_key;
    std::optional<uint32_t> preferred_numa_node;
    // BRK2 field 9 is an absolute Unix timestamp in nanoseconds.
    std::optional<uint64_t> deadline_unix_ns;
};

struct BrokerResponse {
    std::string task_id;
    TaskStatus status = TaskStatus::Error;
    int32_t selected_gpu = -1;
    uint64_t latency_ms = 0;
    std::vector<uint8_t> result;
    std::string error;
    // Encoded in BRK2 responses as optional field 7 ("cpu"/"gpu"/"npu");
    // not encoded in the BRK1 frame.
    BackendKind backend = BackendKind::Cpu;
    bool fell_back_to_cpu = false;
    WorkerClass worker_class = WorkerClass::Throughput;
    uint64_t queue_wait_us = 0;
    uint64_t service_time_us = 0;
    uint32_t worker_id = 0;
    std::optional<uint32_t> worker_numa_node;
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
