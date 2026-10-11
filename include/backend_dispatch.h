// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors

#pragma once
// CPU/GPU/NPU backend selection. This layer runs *before* Placer: it decides
// which kind of backend executes a task; Placer only chooses which GPU once
// the GPU backend has been selected.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace nvlink::broker {

enum class BackendKind : uint8_t {
    Cpu = 0,
    Gpu = 1,
    Npu = 2,
};

inline const char* backend_name(BackendKind kind) noexcept {
    switch (kind) {
        case BackendKind::Cpu: return "cpu";
        case BackendKind::Gpu: return "gpu";
        case BackendKind::Npu: return "npu";
    }
    return "unknown";
}

// Optional, in-process task metadata. A request without a profile is treated as
// a plain CPU task (the BRK1 wire format carries no profile).
struct TaskProfile {
    // Exact / deterministic / verification-sensitive / cryptographic work stays
    // on the CPU unless an explicit deterministic accelerator path is declared.
    bool requires_exact_precision = false;
    bool verification_sensitive = false;
    bool cryptographic = false;
    bool accelerator_verified = false;

    bool is_inference = false;      // suitable for an NPU
    bool is_data_parallel = false;  // suitable for a GPU
    std::size_t estimated_bytes = 0;
    uint64_t estimated_flops = 0;

    // The request may be re-executed on the CPU if an accelerator fails.
    bool fallback_safe = false;
};

struct BackendPolicy {
    BackendKind default_backend = BackendKind::Cpu;
    std::size_t gpu_offload_threshold_bytes = 1024 * 1024;
    uint64_t gpu_offload_threshold_flops = 0;  // 0 disables the flops criterion
    bool force_cpu = false;
    bool gpu_available = false;
    bool npu_available = false;
};

inline bool profile_requires_cpu(const TaskProfile& p) noexcept {
    return (p.requires_exact_precision || p.verification_sensitive || p.cryptographic) &&
           !p.accelerator_verified;
}

inline BackendKind select_backend(const BackendPolicy& policy,
                                  const std::optional<TaskProfile>& profile) noexcept {
    if (policy.force_cpu) {
        return BackendKind::Cpu;
    }
    if (!profile) {
        // Without metadata only an operator-configured GPU default offloads.
        return (policy.default_backend == BackendKind::Gpu && policy.gpu_available)
                   ? BackendKind::Gpu
                   : BackendKind::Cpu;
    }
    if (profile_requires_cpu(*profile)) {
        return BackendKind::Cpu;
    }
    if (profile->is_inference && policy.npu_available) {
        return BackendKind::Npu;
    }
    if (profile->is_data_parallel && policy.gpu_available) {
        const bool big_enough =
            profile->estimated_bytes >= policy.gpu_offload_threshold_bytes ||
            (policy.gpu_offload_threshold_flops > 0 &&
             profile->estimated_flops >= policy.gpu_offload_threshold_flops);
        if (big_enough) {
            return BackendKind::Gpu;
        }
    }
    return BackendKind::Cpu;
}

// Concrete NPU backends implement this. None ships today; when none is
// configured the dispatcher never selects the NPU.
class NpuExecutor {
public:
    virtual ~NpuExecutor() = default;
    virtual bool available() const = 0;
    virtual std::vector<double> run(const std::vector<double>& input) = 0;
};

} // namespace nvlink::broker
