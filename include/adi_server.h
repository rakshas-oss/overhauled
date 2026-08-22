#pragma once
// ADI Protocol Server - GPU-Accelerated Binary Protocol Handler
// Integrates NVLink-aware task placement with the ADI binary protocol.
// Builds on top of nvlink_placement library for intelligent GPU scheduling.

#include "nvlink_placement.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <vector>
#include <thread>
#include <queue>

namespace nvlink::adi {

/// ADI payload is always 5 doubles (40 bytes).
constexpr std::size_t PAYLOAD_DOUBLES = 5;
constexpr std::size_t PAYLOAD_BYTES = PAYLOAD_DOUBLES * sizeof(double);

/// Length prefix is always 40 (big-endian uint32_t).
constexpr uint32_t EXPECTED_LENGTH = static_cast<uint32_t>(PAYLOAD_BYTES);

/// Response packet types.
enum class PacketType : uint8_t {
    Primary = 0,
    Delta = 1,
};

/// Response packet header is 13 bytes: timestamp, sequence id, packet type.
/// Subsequent exchanges send two packets with the same sequence id:
/// Primary contains the full current result and Delta contains
/// current_result - previous_result.
#pragma pack(push, 1)
struct ResponseHeader {
    uint64_t timestamp_ns;  ///< Nanosecond timestamp (big-endian).
    uint32_t sequence_id;   ///< Packet sequence number (big-endian).
    uint8_t packet_type;    ///< See PacketType.
};
#pragma pack(pop)

using GpuPayload = std::vector<double>;

/// Task to be executed on a GPU worker.
struct GpuTask {
    std::promise<GpuPayload> result_promise;
    GpuPayload input_data;
    int target_gpu = 0;
};

/// GPU computation function: input -> output.
/// Users can customize this to implement their own GPU operations.
/// Default implementation scales each element by 2.0.
using GpuComputeFn = std::function<GpuPayload(const GpuPayload&, int gpu_idx)>;

/// Configuration for ADI protocol server.
struct AdiServerConfig {
    /// Port to listen on (default 8080).
    int port = 8080;

    /// Backlog threshold for NVLink-aware placement (default 32).
    std::size_t backlog_threshold = 32;

    /// Custom GPU compute function.
    /// If nullptr, uses the default compute implementation.
    GpuComputeFn gpu_compute_fn = nullptr;

    /// Enable verbose logging.
    bool verbose = false;
};

/// Thread-safe multi-GPU ADI binary protocol server.
class AdiServer {
public:
    /// Constructor.
    explicit AdiServer(const GpuTopology& topology, const AdiServerConfig& config = {});

    /// Destructor - gracefully shuts down server.
    ~AdiServer();

    /// Start the server (blocking until stop() or error).
    void run();

    /// Request graceful shutdown (thread-safe).
    void stop() noexcept;

    /// Check if server is running.
    bool is_running() const noexcept;

    /// Get number of active client connections.
    std::size_t active_clients() const noexcept;

    AdiServer(const AdiServer&) = delete;
    AdiServer& operator=(const AdiServer&) = delete;
    AdiServer(AdiServer&&) = delete;
    AdiServer& operator=(AdiServer&&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/// Host-to-big-endian conversions.
uint32_t host_to_be32(uint32_t n) noexcept;
uint64_t host_to_be64(uint64_t n) noexcept;

/// Big-endian-to-host conversions.
uint32_t be32_to_host(uint32_t n) noexcept;
uint64_t be64_to_host(uint64_t n) noexcept;

/// Default compute function used by the ADI server.
/// Builds with cuBLAS acceleration when available, otherwise uses a CPU fallback.
GpuPayload default_gpu_compute(const GpuPayload& input, int gpu_idx);

} // namespace nvlink::adi
