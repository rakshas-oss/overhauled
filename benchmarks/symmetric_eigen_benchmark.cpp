// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors
// Input: matrix_count dimension followed by matrix_count row-major matrices.
#include "symmetric_eigensolver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    using overhauled::numerics::SymmetricEigenDecomposition;
    using overhauled::numerics::symmetric_eigen_decomposition;
    std::size_t iterations = 8;
    std::size_t workers = 1;
    double tolerance = overhauled::numerics::kDefaultEigenTolerance;
    std::size_t max_sweeps = overhauled::numerics::kDefaultEigenMaxSweeps;
    for (int index = 1; index < argc; ++index) {
        const std::string option(argv[index]);
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << option << "\n";
            return 2;
        }
        const std::string value(argv[++index]);
        try {
            if (option == "--iterations") iterations = std::stoull(value);
            else if (option == "--workers") workers = std::stoull(value);
            else if (option == "--tolerance") tolerance = std::stod(value);
            else if (option == "--max-sweeps") max_sweeps = std::stoull(value);
            else {
                std::cerr << "unknown option " << option << "\n";
                return 2;
            }
        } catch (...) {
            std::cerr << "invalid value for " << option << "\n";
            return 2;
        }
    }
    if (iterations == 0 || iterations > 100000 || workers == 0 || workers > 64) {
        std::cerr << "iterations must be 1..=100000 and workers 1..=64\n";
        return 2;
    }

    std::size_t matrix_count = 0;
    std::size_t dimension = 0;
    if (!(std::cin >> matrix_count >> dimension) || matrix_count == 0 ||
        matrix_count > 1000000 || dimension == 0 ||
        dimension > overhauled::numerics::kMaxEigenDimension) {
        std::cerr << "invalid matrix_count/dimension input header\n";
        return 2;
    }
    std::vector<std::vector<double>> matrices(
        matrix_count, std::vector<double>(dimension * dimension));
    for (auto& matrix : matrices) {
        for (double& value : matrix) {
            if (!(std::cin >> value) || !std::isfinite(value)) {
                std::cerr << "matrix input is truncated or non-finite\n";
                return 2;
            }
        }
    }

    const std::size_t total_tasks = matrix_count * iterations;
    std::atomic<std::size_t> next_task{0};
    std::atomic<uint64_t> checksum{0};
    std::atomic<bool> failed{false};
    std::mutex result_mutex;
    std::mutex error_mutex;
    std::vector<uint64_t> durations_ns;
    durations_ns.reserve(total_tasks);
    double max_residual = 0.0;
    double max_orthogonality = 0.0;
    std::string error;
    const auto started = std::chrono::steady_clock::now();

    std::vector<std::thread> pool;
    const std::size_t active_workers = std::min(workers, total_tasks);
    for (std::size_t worker = 0; worker < active_workers; ++worker) {
        pool.emplace_back([&]() {
            for (;;) {
                const std::size_t task = next_task.fetch_add(1, std::memory_order_relaxed);
                if (task >= total_tasks || failed.load(std::memory_order_relaxed)) break;
                SymmetricEigenDecomposition result;
                std::string local_error;
                const auto before = std::chrono::steady_clock::now();
                const bool ok = symmetric_eigen_decomposition(
                    matrices[task % matrix_count], dimension, &result, &local_error,
                    tolerance, max_sweeps);
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - before).count();
                if (!ok) {
                    failed.store(true, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> guard(error_mutex);
                    if (error.empty()) error = local_error;
                    break;
                }
                uint64_t hash = 0;
                for (double eigenvalue : result.eigenvalues) {
                    uint64_t bits = 0;
                    static_assert(sizeof(bits) == sizeof(eigenvalue), "unexpected double width");
                    std::memcpy(&bits, &eigenvalue, sizeof(bits));
                    hash = (hash << 7U) | (hash >> (64U - 7U));
                    hash ^= bits;
                }
                checksum.fetch_add(hash + static_cast<uint64_t>(task), std::memory_order_relaxed);
                std::lock_guard<std::mutex> guard(result_mutex);
                durations_ns.push_back(static_cast<uint64_t>(elapsed));
                max_residual = std::max(max_residual, result.relative_residual);
                max_orthogonality = std::max(max_orthogonality, result.orthogonality_error);
            }
        });
    }
    for (auto& thread : pool) thread.join();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (failed.load(std::memory_order_relaxed)) {
        std::cerr << "eigensolver failed: " << error << "\n";
        return 1;
    }

    std::sort(durations_ns.begin(), durations_ns.end());
    const double mean_us = durations_ns.empty() ? 0.0 :
        static_cast<double>(std::accumulate(durations_ns.begin(), durations_ns.end(), uint64_t{0})) /
        static_cast<double>(durations_ns.size()) / 1000.0;
    const std::size_t p95_index = durations_ns.empty() ? 0 :
        static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(durations_ns.size()))) - 1;
    const double p95_us = durations_ns.empty() ? 0.0 :
        static_cast<double>(durations_ns[std::min(p95_index, durations_ns.size() - 1)]) / 1000.0;
    const double throughput = static_cast<double>(durations_ns.size()) /
        std::max(elapsed_ms / 1000.0, std::numeric_limits<double>::min());

    std::cout << std::setprecision(17)
              << "{\"matrix_dimension\":" << dimension
              << ",\"matrices\":" << matrix_count
              << ",\"workers\":" << active_workers
              << ",\"iterations_per_matrix\":" << iterations
              << ",\"solves\":" << durations_ns.size()
              << ",\"elapsed_ms\":" << elapsed_ms
              << ",\"solves_per_second\":" << throughput
              << ",\"mean_solve_us\":" << mean_us
              << ",\"p95_solve_us\":" << p95_us
              << ",\"max_relative_residual\":" << max_residual
              << ",\"max_orthogonality_error\":" << max_orthogonality
              << ",\"checksum\":" << checksum.load(std::memory_order_relaxed) << "}\n";
    return 0;
}
