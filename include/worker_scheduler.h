// SPDX-License-Identifier: MIT
// YuKKi-directed class-aware worker pools and Linux NUMA CPU affinity.

#pragma once

#include "broker_protocol.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace nvlink::broker {

struct WorkerTopologySnapshot;

constexpr std::size_t kWorkerClassCount = 5;
// Bounded large-host limits. This supports multi-thousand-core machines without
// allowing an unbounded command-line value to create arbitrary OS threads.
constexpr std::size_t kMaximumWorkerThreads = 16'384;
constexpr std::size_t kMaximumElasticWorkerThreads = 4'096;
constexpr std::size_t kAutomaticWorkerThreadCap = 4'096;
constexpr std::size_t kMaximumWorkerThreadsPerLogicalCpu = 8;
constexpr std::size_t kMaximumWorkerThreadsPerClass = kMaximumWorkerThreads;
// Worker IDs at or above this value identify opt-in elastic worker threads.
constexpr std::size_t kFirstElasticWorkerId =
    kWorkerClassCount * kMaximumWorkerThreadsPerClass;

struct WorkerExecutionContext {
    WorkerClass worker_class = WorkerClass::Throughput;
    uint32_t worker_id = 0;
    uint64_t queue_wait_us = 0;
    std::optional<uint32_t> numa_node;
    // Internal topology detail; not part of the BRK2 wire contract.
    std::optional<uint32_t> cpu_group_id;
};

struct WorkerPoolConfig {
    // Dedicated workers are reserved by class and distributed across the
    // process-allowed NUMA nodes.
    std::array<std::size_t, kWorkerClassCount> workers{{1, 1, 4, 1, 1}};

    // Optional shared worker slots. Elastic workers can serve only
    // RealtimeSpatial, Throughput and Inference; Control and ExactCpu remain
    // strictly dedicated. The per-class limits cap concurrent elastic service.
    std::size_t elastic_workers = 0;
    std::array<std::size_t, kWorkerClassCount> elastic_max_per_class{{0, 1, 4, 0, 2}};

    // Test/simulation seam. The supplied topology must preserve the exact
    // process-discovered root cpuset and affinity capabilities; only its
    // nested NUMA/CPU-group partition may differ.
    std::shared_ptr<const WorkerTopologySnapshot> topology_for_testing;
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
    std::atomic<std::size_t> outstanding_requests_{0};
    std::atomic<std::size_t> outstanding_payload_bytes_{0};
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

    // Real-time spatial tasks may carry an absolute monotonic deadline.
    // Tasks with deadlines are ordered earliest-deadline-first within their
    // isolated class queue; other classes preserve FIFO admission order.
    bool enqueue_with_deadline(
        WorkerClass worker_class,
        std::optional<uint64_t> spatial_affinity_key,
        std::optional<uint32_t> preferred_numa_node,
        std::optional<std::chrono::steady_clock::time_point> deadline,
        Task task);

    // Queues grow dynamically; BrokerServer applies a process-wide admission budget.
    // Stops admission, drains already queued work, and joins all class pools.
    void stop() noexcept;
    void join() noexcept;
    std::size_t queued(WorkerClass worker_class) const noexcept;
    std::size_t queued_on_node(WorkerClass worker_class, uint32_t numa_node) const noexcept;
    // Nodes on which this class has at least one eligible dedicated or
    // elastic worker. Use these IDs when testing or inspecting routing.
    std::vector<uint32_t> queue_nodes(WorkerClass worker_class) const;
    std::size_t active_elastic(WorkerClass worker_class) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nvlink::broker
