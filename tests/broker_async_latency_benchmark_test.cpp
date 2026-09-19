#include "broker_protocol.h"
#include "broker_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

bool read_exact(int socket_fd, void* buffer, std::size_t size) {
    auto* out = static_cast<uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < size) {
        const ssize_t received = ::recv(socket_fd, out + total, size - total, 0);
        if (received <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(received);
    }
    return true;
}

bool write_exact(int socket_fd, const void* buffer, std::size_t size) {
    const auto* in = static_cast<const uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < size) {
        const ssize_t written = ::send(socket_fd, in + total, size - total, 0);
        if (written <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(written);
    }
    return true;
}

std::vector<uint8_t> pack_doubles(const std::array<double, 3>& values) {
    std::vector<uint8_t> payload(values.size() * sizeof(double));
    std::memcpy(payload.data(), values.data(), payload.size());
    return payload;
}

struct BenchmarkResult {
    double wall_ms = 0.0;
    double cpu_ms = 0.0;
    double throughput_rps = 0.0;
    double avg_broker_latency_ms = 0.0;
    double estimated_queue_wait_ms = 0.0;
    std::size_t payload_bytes = 0;
    std::size_t handshake_bytes = 0;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

BenchmarkResult run_case(uint32_t simulated_latency_ms, int port, const std::string& prefix) {
    using namespace nvlink::broker;

    BrokerServerConfig config;
    config.port = port;
    config.max_inflight_per_client = 16;
    config.service_config.force_cpu_fallback = true;
    config.service_config.simulated_latency_ms = simulated_latency_ms;
    config.service_config.protocol_limits.max_frame_bytes = 4096;
    config.service_config.protocol_limits.max_payload_bytes = 1024;

    BrokerServer server(config);
    std::thread server_thread([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    int socket_fd = -1;
    auto cleanup = [&] {
        if (socket_fd >= 0) {
            ::close(socket_fd);
            socket_fd = -1;
        }
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    try {
        socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        require(socket_fd >= 0, "failed to create benchmark client socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::connect(socket_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                "failed to connect benchmark client socket");

        constexpr int kRequestCount = 6;
        std::size_t request_bytes = 0;
        std::size_t response_bytes = 0;
        std::size_t payload_bytes = 0;

        const auto wall_start = Clock::now();
        const std::clock_t cpu_start = std::clock();

        for (int i = 0; i < kRequestCount; ++i) {
            BrokerRequest request;
            request.task_id = prefix + "-" + std::to_string(i);
            request.source = "yukki";
            request.destination = "overhauled";
            request.kind = "inference";
            request.priority = static_cast<uint8_t>(i % 8);
            request.timeout_ms = 2000;
            request.payload = pack_doubles({1.0 + i, 2.0 + i, 3.0 + i});

            const std::vector<uint8_t> frame = encode_request_frame(request, config.service_config.protocol_limits);
            const uint32_t len_be = htonl(static_cast<uint32_t>(frame.size()));
            require(write_exact(socket_fd, &len_be, sizeof(len_be)), "failed to write request length");
            require(write_exact(socket_fd, frame.data(), frame.size()), "failed to write request frame");

            request_bytes += sizeof(uint32_t) + frame.size();
            payload_bytes += request.payload.size();
        }

        std::set<std::string> task_ids;
        uint64_t broker_latency_sum_ms = 0;
        for (int i = 0; i < kRequestCount; ++i) {
            uint32_t frame_len_be = 0;
            require(read_exact(socket_fd, &frame_len_be, sizeof(frame_len_be)), "failed to read response length");
            const std::size_t frame_len = ntohl(frame_len_be);
            require(frame_len > 0 && frame_len <= config.service_config.protocol_limits.max_frame_bytes,
                    "invalid response frame length");
            std::vector<uint8_t> frame(frame_len);
            require(read_exact(socket_fd, frame.data(), frame.size()), "failed to read response frame");

            BrokerResponse response;
            std::string decode_error;
            require(decode_response_frame(frame, &response, &decode_error, config.service_config.protocol_limits),
                    "failed to decode broker response frame");
            require(response.status == TaskStatus::Ok, "broker response status was not ok");
            require(task_ids.insert(response.task_id).second, "duplicate task_id in responses");

            broker_latency_sum_ms += response.latency_ms;
            response_bytes += sizeof(uint32_t) + frame.size();
            payload_bytes += response.result.size();
        }

        const std::clock_t cpu_end = std::clock();
        const auto wall_end = Clock::now();

        cleanup();

        const double wall_ms = std::chrono::duration<double, std::milli>(wall_end - wall_start).count();
        const double cpu_ms = (1000.0 * static_cast<double>(cpu_end - cpu_start)) / static_cast<double>(CLOCKS_PER_SEC);
        const double throughput = (1000.0 * static_cast<double>(kRequestCount)) / wall_ms;
        const double avg_broker_latency_ms = static_cast<double>(broker_latency_sum_ms) / static_cast<double>(kRequestCount);
        const double queue_wait = std::max(0.0, wall_ms - avg_broker_latency_ms);

        BenchmarkResult result;
        result.wall_ms = wall_ms;
        result.cpu_ms = cpu_ms;
        result.throughput_rps = throughput;
        result.avg_broker_latency_ms = avg_broker_latency_ms;
        result.estimated_queue_wait_ms = queue_wait;
        result.payload_bytes = payload_bytes;
        result.handshake_bytes = (request_bytes + response_bytes) - payload_bytes;
        return result;
    } catch (...) {
        cleanup();
        throw;
    }
}

} // namespace

int main() {
    try {
        BenchmarkResult low_latency = run_case(0, 19110, "low");
        BenchmarkResult high_latency = run_case(300, 19111, "high");

        require(high_latency.wall_ms >= 250.0, "300ms simulated latency did not produce expected wall delay");
        require(high_latency.wall_ms > low_latency.wall_ms, "high-latency case was not slower than low-latency case");
        require(high_latency.avg_broker_latency_ms >= 250.0, "broker-reported latency did not include simulated wait");
        require(high_latency.throughput_rps < low_latency.throughput_rps,
                "high-latency throughput was not lower than low-latency throughput");

        const double cpu_idle_ratio = high_latency.cpu_ms / high_latency.wall_ms;
        require(cpu_idle_ratio < 0.65, "high-latency path appears to be busy-spinning");

        std::cout << "broker_async_latency_benchmark_test passed\n";
        std::cout << "low_latency wall_ms=" << low_latency.wall_ms
                  << " queue_ms=" << low_latency.estimated_queue_wait_ms
                  << " broker_ms=" << low_latency.avg_broker_latency_ms
                  << " throughput_rps=" << low_latency.throughput_rps
                  << " payload_bytes=" << low_latency.payload_bytes
                  << " handshake_bytes=" << low_latency.handshake_bytes << "\n";
        std::cout << "high_latency wall_ms=" << high_latency.wall_ms
                  << " queue_ms=" << high_latency.estimated_queue_wait_ms
                  << " broker_ms=" << high_latency.avg_broker_latency_ms
                  << " throughput_rps=" << high_latency.throughput_rps
                  << " payload_bytes=" << high_latency.payload_bytes
                  << " handshake_bytes=" << high_latency.handshake_bytes
                  << " cpu_idle_ratio=" << cpu_idle_ratio << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "broker_async_latency_benchmark_test failed: " << ex.what() << "\n";
        return 1;
    }
}
