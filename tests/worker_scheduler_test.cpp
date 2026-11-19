// SPDX-License-Identifier: MIT
#include "worker_scheduler.h"

#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

int main() {
    using namespace nvlink::broker;
    WorkerClass parsed{};
    assert(parse_worker_class("control", &parsed) && parsed == WorkerClass::Control);
    assert(parse_worker_class("realtime-spatial", &parsed) && parsed == WorkerClass::RealtimeSpatial);
    assert(parse_worker_class("throughput", &parsed) && parsed == WorkerClass::Throughput);
    assert(parse_worker_class("exact-cpu", &parsed) && parsed == WorkerClass::ExactCpu);
    assert(parse_worker_class("inference", &parsed) && parsed == WorkerClass::Inference);
    assert(!parse_worker_class("unknown", &parsed));
    assert(std::string(worker_class_name(WorkerClass::RealtimeSpatial)) == "realtime-spatial");

    // Dynamically growing class queues have a process-wide request/byte
    // budget. The budget remains held until completion, not just dequeue.
    WorkerAdmissionBudget budget(2, 10);
    assert(budget.try_acquire(6));
    assert(budget.try_acquire(4));
    assert(!budget.try_acquire(1)); // request-count and byte budget are full
    assert(budget.outstanding_requests() == 2);
    assert(budget.outstanding_payload_bytes() == 10);
    budget.release(6);
    assert(budget.outstanding_requests() == 1);
    assert(budget.outstanding_payload_bytes() == 4);
    assert(budget.try_acquire(6));
    budget.release(4);
    budget.release(6);
    assert(budget.outstanding_requests() == 0);
    assert(budget.outstanding_payload_bytes() == 0);

    WorkerPoolConfig config;
    config.workers = {{1, 1, 1, 1, 1}};
    ClassWorkerPool pool(config);

    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    std::promise<WorkerExecutionContext> observed;
    auto observed_future = observed.get_future();

    assert(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt,
        [&started, release_future](const WorkerExecutionContext&) mutable {
            started.set_value();
            release_future.wait();
        }));
    assert(started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    assert(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt,
        [&observed](const WorkerExecutionContext& context) { observed.set_value(context); }));
    assert(pool.queued(WorkerClass::RealtimeSpatial) == 1);
    // Dynamic growth admits substantially more than a formerly configured
    // one-entry bound while preserving call submission order.
    std::vector<int> fifo;
    std::mutex fifo_mutex;
    std::promise<void> fifo_done;
    auto fifo_done_future = fifo_done.get_future();
    for (int value = 0; value < 128; ++value) {
        assert(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt, std::nullopt,
            [&fifo, &fifo_mutex, &fifo_done, value](const WorkerExecutionContext&) {
                std::lock_guard<std::mutex> lock(fifo_mutex);
                fifo.push_back(value);
                if (fifo.size() == 128) fifo_done.set_value();
            }));
    }
    assert(pool.queued(WorkerClass::RealtimeSpatial) == 129);

    // A different class has an independent queue and remains admissible.
    std::promise<void> control_done;
    auto control_future = control_done.get_future();
    assert(pool.enqueue(WorkerClass::Control, std::nullopt,
        [&control_done](const WorkerExecutionContext& context) {
            assert(context.worker_class == WorkerClass::Control);
            control_done.set_value();
        }));
    assert(control_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    release.set_value();
    assert(observed_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(fifo_done_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    {
        std::lock_guard<std::mutex> lock(fifo_mutex);
        assert(fifo.size() == 128);
        for (int value = 0; value < 128; ++value) assert(fifo[static_cast<std::size_t>(value)] == value);
    }
    const auto telemetry = observed_future.get();
    assert(telemetry.worker_class == WorkerClass::RealtimeSpatial);
    assert(telemetry.queue_wait_us <= 2'000'000);

    pool.stop();
    assert(!pool.enqueue(WorkerClass::Control, std::nullopt,
        [](const WorkerExecutionContext&) {}));
    pool.join();

    std::cout << "worker_scheduler_test passed\n";
    return 0;
}
