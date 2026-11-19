// SPDX-License-Identifier: MIT
// YuKKi-directed class-aware worker pools and Linux NUMA CPU affinity.

#pragma once

#include "broker_protocol.h"

#include <array>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
};


/// Process-wide admission budget for dynamically growing class queues.
/// Requests hold a slot and their payload-byte reservation until completion,
/// including after the client socket disconnects.
class WorkerAdmissionBudget {
public:
    WorkerAdmissionBudget(std::size_t max_outstanding_requests,
                          std::size_t max_payload_bytes);

    bool try_acquire(std::size_t payload_bytes) noexcept;
    void release(std::size_t payload_bytes) noexcept;
    std::size_t outstanding_requests() const noexcept;
    std::size_t outstanding_payload_bytes() const noexcept;

private:
    const std::size_t max_outstanding_requests_;
    const std::size_t max_payload_bytes_;
    mutable std::mutex mutex_;
    std::size_t outstanding_requests_ = 0;
    std::size_t outstanding_payload_bytes_ = 0;
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
                 std::optional<std::chrono::steady_clock::time_point> deadline,
                 Task task);

    // Queues grow dynamically; the process memory limit is the ultimate bound.
    // Stops admission, drains already queued work, and joins all class pools.
    void stop() noexcept;
    void join() noexcept;
    std::size_t queued(WorkerClass worker_class) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nvlink::broker
