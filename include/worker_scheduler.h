// SPDX-License-Identifier: MIT
// YuKKi-directed class-aware worker pools and Linux NUMA CPU affinity.

#pragma once

#include "broker_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace nvlink::broker {

constexpr std::size_t kWorkerClassCount = 5;

struct WorkerExecutionContext {
    WorkerClass worker_class = WorkerClass::Throughput;
    uint32_t worker_id = 0;
    uint64_t queue_wait_us = 0;
    std::optional<uint32_t> numa_node;
};

struct WorkerPoolConfig {
    // One independently bounded FIFO and worker pool per workload class.
    std::array<std::size_t, kWorkerClassCount> workers{{1, 1, 4, 1, 1}};
    std::size_t max_queue_per_class = 4096;
};

class ClassWorkerPool {
public:
    using Task = std::function<void(const WorkerExecutionContext&)>;

    explicit ClassWorkerPool(const WorkerPoolConfig& config);
    ~ClassWorkerPool();

    ClassWorkerPool(const ClassWorkerPool&) = delete;
    ClassWorkerPool& operator=(const ClassWorkerPool&) = delete;

    bool enqueue(WorkerClass worker_class,
                 std::optional<uint64_t> spatial_affinity_key,
                 std::optional<uint32_t> preferred_numa_node,
                 Task task);

    // Stops admission, drains already queued work, and joins all class pools.
    void stop() noexcept;
    std::size_t queued(WorkerClass worker_class) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nvlink::broker
