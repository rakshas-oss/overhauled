#pragma once
// ADI Protocol Server - GPU-Accelerated Binary Protocol Handler
// Integrates NVLink-aware task placement with the ADI binary protocol.
// Builds on top of nvlink_placement library for intelligent GPU scheduling.

#include "nvlink_placement.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <queue>

namespace nvlink::adi {

// ============================================================================
// ADI Binary Protocol Definitions
// ============================================================================

/// ADI payload is always 5 doubles (40 bytes)
constexpr size_t PAYLOAD_DOUBLES = 5;
constexpr size_t PAYLOAD_BYTES = PAYLOAD_DOUBLES * sizeof(double);

/// Length prefix is always 40 (big-endian uint32_t)
constexpr uint32_t EXPECTED_LENGTH = PAYLOAD_BYTES;

/// Response packet header: 13 bytes
#pragma pack(push, 1)
struct ResponseHeader {
    uint64_t timestamp_ns;  ///< Nanosecond timestamp (big-endian)
    uint32_t sequence_id;   ///< Packet sequence number (big-endian)
    uint8_t packet_type;    ///< 0=Primary, 1=Delta
};
#pragma pack(pop)

// ============================================================================
// GPU Task and Processing
// ============================================================================

using GpuPayload = std::vector<double>;

/// Task to be executed on a GPU
struct GpuTask {
    std::promise<GpuPayload> result_promise;
    GpuPayload input_data;
    int target_gpu;
};

/// GPU computation function: input → output
/// Users can customize this to implement their own GPU operations.
/// Default implementation: element-wise multiply by 2.0 using cuBLAS
using GpuComputeFn = std::function<GpuPayload(const GpuPayload&, int gpu_idx)>;

// ============================================================================
// ADI Server Configuration
// ============================================================================

/// Configuration for ADI protocol server
struct AdiServerConfig {
    /// Port to listen on (default 8080)
    int port = 8080;

    /// Backlog threshold for NVLink-aware placement (default 32)
    size_t backlog_threshold = 32;

    /// Custom GPU compute function
    /// If nullptr, uses default cuBLAS-based scaling
    GpuComputeFn gpu_compute_fn = nullptr;

    /// Enable verbose logging
    bool verbose = false;
};

// ============================================================================
// ADI Server - Thread-Safe Multi-GPU Server
// ============================================================================

class AdiServer {
public:
    /// Constructor
    /// @param topology Detected GPU topology (typically from GpuTopology::detect())
    /// @param config Server configuration
    explicit AdiServer(const GpuTopology& topology, const AdiServerConfig& config = {});

    /// Destructor - gracefully shuts down server
    ~AdiServer();

    /// Start the server (blocking until stop() or error)
    /// Spawns GPU worker threads and TCP listener
    void run();

    /// Request graceful shutdown (thread-safe)
    void stop() noexcept;

    /// Check if server is running
    bool is_running() const noexcept;

    /// Get number of active client connections
    size_t active_clients() const noexcept;

    AdiServer(const AdiServer&) = delete;
    AdiServer& operator=(const AdiServer&) = delete;
    AdiServer(AdiServer&&) = delete;
    AdiServer& operator=(AdiServer&&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

// ============================================================================
// ADI Protocol Utilities
// ============================================================================

/// Host-to-big-endian conversions
uint32_t host_to_be32(uint32_t n) noexcept;
uint64_t host_to_be64(uint64_t n) noexcept;

/// Big-endian-to-host conversions
uint32_t be32_to_host(uint32_t n) noexcept;
uint64_t be64_to_host(uint64_t n) noexcept;

/// Default GPU compute function (cuBLAS-based scaling)
/// Multiplies input vector by 2.0 on specified GPU
GpuPayload default_gpu_compute(const GpuPayload& input, int gpu_idx);

} // namespace nvlink::adi

#endif // ADI_SERVER_H
