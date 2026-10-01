#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nvlink::wasm {

/// Magic bytes for WSM1 binary wire format ("WSM1" in ASCII).
constexpr uint32_t WASM_MAGIC = 0x57534D31U;

/// Current WSM1 wire protocol version.
constexpr uint16_t WASM_PROTOCOL_VERSION = 1;

/// Standard BrokerRequest::kind string for WASM task execution.
constexpr const char* kWasmTaskKind = "wasm.task.v1";

/// Standard BrokerRequest::kind strings for WASM lifecycle operations.
constexpr const char* kWasmLifecycleKind = "wasm.lifecycle.v1";
constexpr const char* kWasmLifecyclePrepareKind = "wasm.lifecycle.prepare.v1";
constexpr const char* kWasmLifecycleDrainKind = "wasm.lifecycle.drain.v1";
constexpr const char* kWasmLifecycleReleaseKind = "wasm.lifecycle.release.v1";
constexpr const char* kWasmLifecycleQueryKind = "wasm.lifecycle.query.v1";

/// WSM1 message types.
enum class WasmMessageType : uint8_t {
    TaskRequest = 1,
    TaskResponse = 2,
    LifecycleRequest = 3,
    LifecycleResponse = 4,
};

/// Status of a WASM task execution.
enum class WasmTaskStatus : uint8_t {
    Ok = 0,
    Rejected = 1,
    Error = 2,
    Timeout = 3,
    ModuleDraining = 4,
    ModuleNotFound = 5,
    VersionMismatch = 6,
};

/// Lifecycle actions for WASM module versions.
enum class WasmLifecycleAction : uint8_t {
    Prepare = 1,   // Register and prepare new module version
    Drain = 2,     // Stop accepting new tasks on old module version
    Release = 3,   // Release GPU/broker resources after drain complete
    Query = 4,     // Query status of module version
};

/// Status of a WASM lifecycle operation.
enum class WasmLifecycleStatus : uint8_t {
    Ok = 0,
    Rejected = 1,
    Error = 2,
    Busy = 3,
    NotFound = 4,
};

/// Runtime state of a registered WASM module version.
enum class WasmModuleState : uint8_t {
    Unknown = 0,
    Prepared = 1,
    Active = 2,
    Draining = 3,
    Stopped = 4,
    Released = 5,
};

/// Buffer access permissions.
enum class WasmBufferAccess : uint32_t {
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
};

/// Descriptor for WASM sandbox input/output buffers.
struct WasmBufferDescriptor {
    uint32_t buffer_id = 0;
    uint32_t flags = static_cast<uint32_t>(WasmBufferAccess::ReadWrite);
    uint64_t offset = 0;
    uint64_t length = 0;
    std::string name;
};

/// Protocol validation limits for the WASM wire format.
struct WasmLimits {
    std::size_t max_string_bytes = 1024;
    std::size_t max_payload_bytes = 512 * 1024;
    std::size_t max_buffers = 64;
    std::size_t max_frame_bytes = 1024 * 1024;
};

/// Task submission request from a WASM sandbox / host adapter.
struct WasmTaskRequest {
    std::string task_id;             // Idempotency and task correlation ID
    std::string sandbox_id;          // Sandbox identity
    std::string module_id;           // Module identity
    std::string module_version;      // Module version string (e.g. "1.0.0")
    std::string task_kind;           // Workload kind (e.g. "inference", "compute")
    uint8_t priority = 0;            // Task priority (0-255)
    uint64_t deadline_ms = 0;        // Deadline timestamp or relative timeout in ms
    std::vector<WasmBufferDescriptor> buffer_descriptors;
    std::vector<uint8_t> payload;    // Task input payload
};

/// Task completion response returned to the WASM host adapter.
struct WasmTaskResponse {
    std::string task_id;
    WasmTaskStatus status = WasmTaskStatus::Error;
    uint16_t protocol_version = WASM_PROTOCOL_VERSION;
    int32_t selected_gpu = -1;       // -1 indicates CPU fallback
    uint64_t latency_ms = 0;
    std::vector<WasmBufferDescriptor> buffer_descriptors;
    std::vector<uint8_t> result;
    std::string error;
};

/// Lifecycle request for module registration, drainage, and release.
struct WasmLifecycleRequest {
    WasmLifecycleAction action = WasmLifecycleAction::Prepare;
    std::string request_id;          // Idempotency and correlation ID
    std::string sandbox_id;
    std::string module_id;
    std::string module_version;
    int32_t target_gpu = -1;         // Target GPU preference (-1 = broker placement / auto)
    uint32_t grace_period_ms = 0;    // Grace period for drain operations
    std::string ack_token;           // Acknowledgment/lease token for release operations
    std::vector<uint8_t> payload;    // Optional configuration or initialization data
};

/// Response to a lifecycle operation.
struct WasmLifecycleResponse {
    WasmLifecycleAction action = WasmLifecycleAction::Prepare;
    WasmLifecycleStatus status = WasmLifecycleStatus::Error;
    uint16_t protocol_version = WASM_PROTOCOL_VERSION;
    std::string request_id;
    std::string sandbox_id;
    std::string module_id;
    std::string module_version;
    int32_t assigned_gpu = -1;
    uint32_t active_tasks = 0;       // Current in-flight task count
    WasmModuleState state = WasmModuleState::Unknown;
    std::string lease_token;
    std::string error;
};

// Wire format encoding and decoding functions.
bool encode_wasm_task_request(const WasmTaskRequest& req,
                              std::vector<uint8_t>* out,
                              std::string* error,
                              const WasmLimits& limits = {});

bool decode_wasm_task_request(const std::vector<uint8_t>& frame,
                              WasmTaskRequest* out,
                              std::string* error,
                              const WasmLimits& limits = {});

bool encode_wasm_task_response(const WasmTaskResponse& resp,
                               std::vector<uint8_t>* out,
                               std::string* error,
                               const WasmLimits& limits = {});

bool decode_wasm_task_response(const std::vector<uint8_t>& frame,
                               WasmTaskResponse* out,
                               std::string* error,
                               const WasmLimits& limits = {});

bool encode_wasm_lifecycle_request(const WasmLifecycleRequest& req,
                                   std::vector<uint8_t>* out,
                                   std::string* error,
                                   const WasmLimits& limits = {});

bool decode_wasm_lifecycle_request(const std::vector<uint8_t>& frame,
                                   WasmLifecycleRequest* out,
                                   std::string* error,
                                   const WasmLimits& limits = {});

bool encode_wasm_lifecycle_response(const WasmLifecycleResponse& resp,
                                    std::vector<uint8_t>* out,
                                    std::string* error,
                                    const WasmLimits& limits = {});

bool decode_wasm_lifecycle_response(const std::vector<uint8_t>& frame,
                                    WasmLifecycleResponse* out,
                                    std::string* error,
                                    const WasmLimits& limits = {});

// Helper function to test if a BrokerRequest::kind represents a WASM request.
bool is_wasm_request_kind(const std::string& kind) noexcept;

// String conversions for diagnostics and logging.
const char* wasm_task_status_name(WasmTaskStatus status) noexcept;
const char* wasm_lifecycle_action_name(WasmLifecycleAction action) noexcept;
const char* wasm_lifecycle_status_name(WasmLifecycleStatus status) noexcept;
const char* wasm_module_state_name(WasmModuleState state) noexcept;

/// Thread-safe manager for WASM module versions and hotswap lifecycle tracking.
class WasmSandboxManager {
public:
    using GpuSelectorFn = std::function<int32_t(const std::string& source, int32_t target_gpu)>;
    using ComputeFn = std::function<std::vector<uint8_t>(int32_t gpu, const WasmTaskRequest&)>;

    explicit WasmSandboxManager(bool force_cpu = false, GpuSelectorFn gpu_selector = nullptr);
    ~WasmSandboxManager() = default;

    // Direct lifecycle dispatch
    WasmLifecycleResponse handle_lifecycle(const WasmLifecycleRequest& request);
    WasmLifecycleResponse prepare_module(const WasmLifecycleRequest& request);
    WasmLifecycleResponse drain_module(const WasmLifecycleRequest& request);
    WasmLifecycleResponse release_module(const WasmLifecycleRequest& request);
    WasmLifecycleResponse query_module(const WasmLifecycleRequest& request);

    // Task dispatch with version checking and in-flight tracking
    WasmTaskResponse handle_task(const WasmTaskRequest& request, ComputeFn compute_fn = nullptr);

    // State query helpers
    bool is_module_active(const std::string& sandbox_id,
                          const std::string& module_id,
                          const std::string& version) const;
    WasmModuleState get_module_state(const std::string& sandbox_id,
                                     const std::string& module_id,
                                     const std::string& version) const;
    uint32_t get_active_tasks(const std::string& sandbox_id,
                              const std::string& module_id,
                              const std::string& version) const;

    void set_force_cpu(bool force_cpu) noexcept;
    bool force_cpu() const noexcept;
    void set_gpu_selector(GpuSelectorFn gpu_selector);

private:
    struct ModuleRecord {
        std::string sandbox_id;
        std::string module_id;
        std::string version;
        WasmModuleState state = WasmModuleState::Prepared;
        int32_t assigned_gpu = -1;
        std::string lease_token;
        uint32_t active_tasks = 0;
    };

    static std::string make_key(const std::string& sandbox_id,
                                const std::string& module_id,
                                const std::string& version);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ModuleRecord> modules_;
    bool force_cpu_ = false;
    GpuSelectorFn gpu_selector_;
    uint64_t next_lease_id_ = 1;
};

} // namespace nvlink::wasm
