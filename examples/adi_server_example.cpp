#include "adi_server.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <vector>

using nvlink::GpuTopology;
using nvlink::adi::AdiServer;
using nvlink::adi::AdiServerConfig;
using nvlink::adi::GpuPayload;

namespace {

std::atomic<AdiServer*> g_server{nullptr};

void handle_signal(int) {
    if (AdiServer* server = g_server.load(std::memory_order_acquire)) {
        server->stop();
    }
}

} // namespace

int main() {
    try {
        GpuTopology topology = GpuTopology::detect();
        topology.print_info();

        AdiServerConfig config;
        config.port = 8080;
        config.backlog_threshold = 32;
        config.verbose = true;
        config.gpu_compute_fn = [](const GpuPayload& input, int gpu_idx) {
            auto output = nvlink::adi::default_gpu_compute(input, gpu_idx);
            for (double& value : output) {
                value += 1.0;
            }
            return output;
        };

        AdiServer server(topology, config);
        g_server.store(&server, std::memory_order_release);

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        std::cout << "ADI server listening on port " << config.port
                  << " with NVLink-aware placement." << std::endl;
        std::cout << "Send 4-byte big-endian length + 5 big-endian doubles per request." << std::endl;
        std::cout << "Press Ctrl+C to stop." << std::endl;

        server.run();
        g_server.store(nullptr, std::memory_order_release);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ADI server example failed: " << ex.what() << std::endl;
        return 1;
    }
}
