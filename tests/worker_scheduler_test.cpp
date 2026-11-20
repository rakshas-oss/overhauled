// SPDX-License-Identifier: MIT
#include "worker_scheduler.h"

#include <cstdlib>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' << __LINE__ << '\\n'; \
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

    std::cout << "worker_scheduler_test passed\n";
    return 0;
}
