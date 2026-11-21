// SPDX-License-Identifier: MIT
#include "worker_scheduler.h"
#include "worker_topology.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while (false)

int main() {
    using namespace nvlink::broker;
    WorkerClass parsed{};
    CHECK(parse_worker_class("control", &parsed) && parsed == WorkerClass::Control);
    CHECK(parse_worker_class("realtime-spatial", &parsed) && parsed == WorkerClass::RealtimeSpatial);
    CHECK(parse_worker_class("throughput", &parsed) && parsed == WorkerClass::Throughput);
    CHECK(parse_worker_class("exact-cpu", &parsed) && parsed == WorkerClass::ExactCpu);
    CHECK(parse_worker_class("inference", &parsed) && parsed == WorkerClass::Inference);
    CHECK(!parse_worker_class("unknown", &parsed));
    CHECK(std::string(worker_class_name(WorkerClass::RealtimeSpatial)) == "realtime-spatial");

    // Dynamically growing class queues have a process-wide request/byte
    // budget. The budget remains held until completion, not just dequeue.
    WorkerAdmissionBudget budget(2, 10);
    CHECK(budget.try_acquire(6));
    CHECK(budget.try_acquire(4));
    CHECK(!budget.try_acquire(1)); // request-count and byte budget are full
    CHECK(budget.outstanding_requests() == 2);
    CHECK(budget.outstanding_payload_bytes() == 10);
    budget.release(6);
    CHECK(budget.outstanding_requests() == 1);
    CHECK(budget.outstanding_payload_bytes() == 4);
    CHECK(budget.try_acquire(6));
    budget.release(4);
    budget.release(6);
    CHECK(budget.outstanding_requests() == 0);
    CHECK(budget.outstanding_payload_bytes() == 0);

    WorkerPoolConfig config;
    config.workers = {{1, 1, 1, 1, 1}};
    ClassWorkerPool pool(config);

    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    std::promise<WorkerExecutionContext> observed;
    auto observed_future = observed.get_future();

    CHECK(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt,
        [&started, release_future](const WorkerExecutionContext&) mutable {
            started.set_value();
            release_future.wait();
        }));
    CHECK(started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    CHECK(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt,
        [&observed](const WorkerExecutionContext& context) { observed.set_value(context); }));
    CHECK(pool.queued(WorkerClass::RealtimeSpatial) == 1);
    // Dynamic growth admits substantially more than a formerly configured
    // one-entry bound while preserving call submission order.
    std::vector<int> fifo;
    std::mutex fifo_mutex;
    std::promise<void> fifo_done;
    auto fifo_done_future = fifo_done.get_future();
    for (int value = 0; value < 128; ++value) {
        CHECK(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt,
            [&fifo, &fifo_mutex, &fifo_done, value](const WorkerExecutionContext&) {
                std::lock_guard<std::mutex> lock(fifo_mutex);
                fifo.push_back(value);
                if (fifo.size() == 128) fifo_done.set_value();
            }));
    }
    CHECK(pool.queued(WorkerClass::RealtimeSpatial) == 129);

    // A different class has an independent queue and remains admissible.
    std::promise<void> control_done;
    auto control_future = control_done.get_future();
    CHECK(pool.enqueue(WorkerClass::Control, std::nullopt, std::nullopt,
        [&control_done](const WorkerExecutionContext& context) {
            CHECK(context.worker_class == WorkerClass::Control);
            control_done.set_value();
        }));
    CHECK(control_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    release.set_value();
    CHECK(observed_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(fifo_done_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    {
        std::lock_guard<std::mutex> lock(fifo_mutex);
        CHECK(fifo.size() == 128);
        for (int value = 0; value < 128; ++value) CHECK(fifo[static_cast<std::size_t>(value)] == value);
    }
    const auto telemetry = observed_future.get();
    CHECK(telemetry.worker_class == WorkerClass::RealtimeSpatial);
    CHECK(telemetry.queue_wait_us <= 2'000'000);

    pool.stop();
    CHECK(!pool.enqueue(WorkerClass::Control, std::nullopt, std::nullopt,
        [](const WorkerExecutionContext&) {}));
    pool.join();


    // Real-time spatial queue is earliest-deadline-first.
    WorkerPoolConfig deadline_config;
    deadline_config.workers = {{1, 1, 1, 1, 1}};
    ClassWorkerPool deadline_pool(deadline_config);
    std::promise<void> deadline_started;
    auto deadline_started_future = deadline_started.get_future();
    std::promise<void> deadline_release;
    auto deadline_release_future = deadline_release.get_future().share();
    CHECK(deadline_pool.enqueue(WorkerClass::RealtimeSpatial, 0x55, std::nullopt,
        [&deadline_started, deadline_release_future](const WorkerExecutionContext&) {
            deadline_started.set_value();
            deadline_release_future.wait();
        }));
    CHECK(deadline_started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    std::mutex deadline_mutex;
    std::vector<int> deadline_order;
    std::promise<void> deadline_done;
    auto deadline_done_future = deadline_done.get_future();
    const auto deadline_now = std::chrono::steady_clock::now();
    auto record_deadline = [&deadline_mutex, &deadline_order, &deadline_done](int value) {
        std::lock_guard<std::mutex> lock(deadline_mutex);
        deadline_order.push_back(value);
        if (deadline_order.size() == 2) deadline_done.set_value();
    };
    CHECK(deadline_pool.enqueue_with_deadline(
        WorkerClass::RealtimeSpatial, 0x55, std::nullopt,
        deadline_now + std::chrono::seconds(60),
        [record_deadline](const WorkerExecutionContext&) { record_deadline(1); }));
    CHECK(deadline_pool.enqueue_with_deadline(
        WorkerClass::RealtimeSpatial, 0x55, std::nullopt,
        deadline_now + std::chrono::seconds(30),
        [record_deadline](const WorkerExecutionContext&) { record_deadline(2); }));
    deadline_release.set_value();
    CHECK(deadline_done_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    {
        std::lock_guard<std::mutex> lock(deadline_mutex);
        CHECK(deadline_order.size() == 2);
        CHECK(deadline_order[0] == 2 && deadline_order[1] == 1);
    }
    deadline_pool.stop();
    deadline_pool.join();

    // Preferred NUMA targets route into a queue owned by that node. When the
    // host cannot apply affinity, execution still completes but reports no pin.
    WorkerPoolConfig routing_config;
    routing_config.workers = {{1, 1, 1, 1, 1}};
    ClassWorkerPool routing_pool(routing_config);
    const auto realtime_nodes = routing_pool.queue_nodes(WorkerClass::RealtimeSpatial);
    CHECK(!realtime_nodes.empty());
    const uint32_t realtime_node = realtime_nodes.front();
    std::promise<void> local_started;
    auto local_started_future = local_started.get_future();
    std::promise<void> local_release;
    auto local_release_future = local_release.get_future().share();
    std::promise<void> local_second_done;
    auto local_second_future = local_second_done.get_future();
    CHECK(routing_pool.enqueue(WorkerClass::RealtimeSpatial, 0x991, realtime_node,
        [&local_started, local_release_future, realtime_node]
        (const WorkerExecutionContext& context) mutable {
            CHECK(!context.numa_node || *context.numa_node == realtime_node);
            local_started.set_value();
            local_release_future.wait();
        }));
    CHECK(local_started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(routing_pool.enqueue(WorkerClass::RealtimeSpatial, 0x991, realtime_node,
        [&local_second_done, realtime_node](const WorkerExecutionContext& context) {
            CHECK(!context.numa_node || *context.numa_node == realtime_node);
            local_second_done.set_value();
        }));
    CHECK(routing_pool.queued_on_node(WorkerClass::RealtimeSpatial, realtime_node) == 1);
    local_release.set_value();
    CHECK(local_second_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    routing_pool.stop();
    routing_pool.join();

    // Elastic workers add bounded capacity to RealtimeSpatial, Throughput and
    // Inference only. The Throughput ceiling below permits at most two
    // simultaneous elastic executions regardless of how many slots are idle.
    WorkerPoolConfig elastic_config;
    elastic_config.workers = {{1, 1, 1, 1, 1}};
    elastic_config.elastic_workers = 4;
    elastic_config.elastic_max_per_class = {{0, 1, 2, 0, 1}};
    ClassWorkerPool elastic_pool(elastic_config);
    const auto throughput_nodes = elastic_pool.queue_nodes(WorkerClass::Throughput);
    CHECK(!throughput_nodes.empty());

    std::promise<void> two_elastic_started;
    auto two_elastic_started_future = two_elastic_started.get_future();
    std::promise<void> elastic_release;
    auto elastic_release_future = elastic_release.get_future().share();
    std::promise<void> elastic_tasks_done;
    auto elastic_tasks_done_future = elastic_tasks_done.get_future();
    std::atomic<std::size_t> elastic_started{0};
    std::atomic<std::size_t> elastic_active{0};
    std::atomic<std::size_t> elastic_peak{0};
    std::atomic<std::size_t> completed{0};
    constexpr std::size_t kElasticTestTasks = 32;
    for (std::size_t value = 0; value < kElasticTestTasks; ++value) {
        const uint32_t node = throughput_nodes[value % throughput_nodes.size()];
        CHECK(elastic_pool.enqueue(WorkerClass::Throughput, value, node,
            [&two_elastic_started, elastic_release_future, &elastic_started,
             &elastic_active, &elastic_peak, &completed, &elastic_tasks_done]
            (const WorkerExecutionContext& context) mutable {
                CHECK(context.worker_class == WorkerClass::Throughput);
                if (context.worker_id >= kFirstElasticWorkerId) {
                    const std::size_t now = elastic_active.fetch_add(1) + 1;
                    std::size_t peak = elastic_peak.load();
                    while (now > peak && !elastic_peak.compare_exchange_weak(peak, now)) {}
                    if (elastic_started.fetch_add(1) + 1 == 2)
                        two_elastic_started.set_value();
                }
                elastic_release_future.wait();
                if (context.worker_id >= kFirstElasticWorkerId) elastic_active.fetch_sub(1);
                if (completed.fetch_add(1) + 1 == kElasticTestTasks)
                    elastic_tasks_done.set_value();
            }));
    }
    CHECK(two_elastic_started_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(elastic_pool.active_elastic(WorkerClass::Throughput) == 2);
    CHECK(elastic_pool.active_elastic(WorkerClass::Control) == 0);
    CHECK(elastic_pool.active_elastic(WorkerClass::ExactCpu) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(elastic_started.load() == 2);
    CHECK(elastic_peak.load() == 2);
    elastic_release.set_value();
    CHECK(elastic_tasks_done_future.wait_for(std::chrono::seconds(3)) ==
          std::future_status::ready);
    elastic_pool.stop();
    elastic_pool.join();

    // Even while a dedicated Control worker is occupied, elastic slots do not
    // steal its second request; the reserved Control queue remains isolated.
    WorkerPoolConfig isolation_config;
    isolation_config.workers = {{1, 1, 1, 1, 1}};
    isolation_config.elastic_workers = 2;
    ClassWorkerPool isolation_pool(isolation_config);
    const auto control_nodes = isolation_pool.queue_nodes(WorkerClass::Control);
    CHECK(!control_nodes.empty());
    const uint32_t control_node = control_nodes.front();
    std::promise<void> control_started;
    auto control_started_future = control_started.get_future();
    std::promise<void> control_release;
    auto control_release_future = control_release.get_future().share();
    std::promise<void> second_control_done;
    auto second_control_future = second_control_done.get_future();
    CHECK(isolation_pool.enqueue(WorkerClass::Control, std::nullopt, control_node,
        [&control_started, control_release_future](const WorkerExecutionContext&) mutable {
            control_started.set_value();
            control_release_future.wait();
        }));
    CHECK(control_started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(isolation_pool.enqueue(WorkerClass::Control, std::nullopt, control_node,
        [&second_control_done](const WorkerExecutionContext& context) {
            CHECK(context.worker_id < kFirstElasticWorkerId);
            second_control_done.set_value();
        }));
    CHECK(isolation_pool.queued_on_node(WorkerClass::Control, control_node) == 1);
    CHECK(isolation_pool.active_elastic(WorkerClass::Control) == 0);
    control_release.set_value();
    CHECK(second_control_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    isolation_pool.stop();
    isolation_pool.join();

    // Protected classes cannot be assigned elastic execution ceilings.
    WorkerPoolConfig invalid_elastic_config;
    invalid_elastic_config.elastic_workers = 1;
    invalid_elastic_config.elastic_max_per_class[static_cast<std::size_t>(WorkerClass::Control)] = 1;
    bool rejected_control_elastic = false;
    try { ClassWorkerPool invalid_pool(invalid_elastic_config); }
    catch (const std::invalid_argument&) { rejected_control_elastic = true; }
    CHECK(rejected_control_elastic);

    // Hierarchical CPU-group queues: occupy both sibling leaves, then
    // release the remote-home worker and verify it steals from its sibling.
    // The synthetic hierarchy must preserve the real process cpuset exactly.
    const WorkerTopologySnapshot actual_topology = discover_worker_topology();
    if (actual_topology.allowed_cpus.size() >= 3) {
        WorkerTopologySnapshot synthetic = actual_topology;
        const std::vector<uint32_t> group_zero{
            actual_topology.allowed_cpus[0], actual_topology.allowed_cpus[1]};
        std::vector<uint32_t> group_one(
            actual_topology.allowed_cpus.begin() + 2,
            actual_topology.allowed_cpus.end());
        synthetic.numa_nodes = {
            NumaNodeTopology{5000, actual_topology.allowed_cpus, {
                CpuGroupTopology{17, group_zero},
                CpuGroupTopology{19, group_one},
            }},
        };
        std::string topology_error;
        CHECK(validate_worker_topology(synthetic, &topology_error));

        WorkerPoolConfig nested_config;
        nested_config.workers = {{1, 2, 1, 1, 1}};
        nested_config.topology_for_testing =
            std::make_shared<const WorkerTopologySnapshot>(synthetic);
        ClassWorkerPool nested_pool(nested_config);

        constexpr uint32_t kRealtimeWorkerBase =
            static_cast<uint32_t>(WorkerClass::RealtimeSpatial) *
            static_cast<uint32_t>(kMaximumWorkerThreadsPerClass);
        std::promise<void> worker_zero_started;
        auto worker_zero_started_future = worker_zero_started.get_future();
        std::promise<void> worker_one_started;
        auto worker_one_started_future = worker_one_started.get_future();
        std::promise<void> release_worker_zero;
        auto release_worker_zero_future = release_worker_zero.get_future().share();
        std::promise<void> release_worker_one;
        auto release_worker_one_future = release_worker_one.get_future().share();
        std::promise<WorkerExecutionContext> stolen_task_done;
        auto stolen_task_future = stolen_task_done.get_future();

        auto block_assigned_worker =
            [&worker_zero_started, &worker_one_started,
             release_worker_zero_future, release_worker_one_future,
             kRealtimeWorkerBase](const WorkerExecutionContext& context) mutable {
                if (context.worker_id == kRealtimeWorkerBase) {
                    worker_zero_started.set_value();
                    release_worker_zero_future.wait();
                } else if (context.worker_id == kRealtimeWorkerBase + 1) {
                    worker_one_started.set_value();
                    release_worker_one_future.wait();
                } else {
                    std::abort();
                }
            };

        // Affinity key 0 selects CPU group 17; key 1 selects sibling group 19.
        CHECK(nested_pool.enqueue(WorkerClass::RealtimeSpatial, 0, std::nullopt,
                                  block_assigned_worker));
        // The scheduler may wake either sibling worker first. Record which
        // one took the group-17 task, then occupy the other with group 19.
        const auto first_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(2);
        while (worker_zero_started_future.wait_for(std::chrono::milliseconds(0)) !=
                   std::future_status::ready &&
               worker_one_started_future.wait_for(std::chrono::milliseconds(0)) !=
                   std::future_status::ready &&
               std::chrono::steady_clock::now() < first_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool worker_zero_was_first =
            worker_zero_started_future.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready;
        const bool worker_one_was_first =
            worker_one_started_future.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready;
        CHECK(worker_zero_was_first != worker_one_was_first);
        CHECK(nested_pool.enqueue(WorkerClass::RealtimeSpatial, 1, std::nullopt,
                                  block_assigned_worker));
        auto& remaining_started = worker_zero_was_first
            ? worker_one_started_future : worker_zero_started_future;
        CHECK(remaining_started.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);

        // Both dedicated RealtimeSpatial workers are now occupied. This work
        // is queued to group 17; releasing the group-19 worker must steal it
        // within the shared NUMA parent, while the other worker stays busy.
        CHECK(nested_pool.enqueue(WorkerClass::RealtimeSpatial, 0, std::nullopt,
            [&stolen_task_done](const WorkerExecutionContext& context) {
                stolen_task_done.set_value(context);
            }));
        CHECK(nested_pool.queued_on_node(WorkerClass::RealtimeSpatial, 5000) == 1);
        release_worker_one.set_value();
        CHECK(stolen_task_future.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
        const auto stolen_context = stolen_task_future.get();
        CHECK(stolen_context.worker_id == kRealtimeWorkerBase + 1);
        if (stolen_context.cpu_group_id)
            CHECK(*stolen_context.cpu_group_id == 19);

        release_worker_zero.set_value();
        nested_pool.stop();
        nested_pool.join();
    }

    std::cout << "worker_scheduler_test passed\n";
    return 0;
}
