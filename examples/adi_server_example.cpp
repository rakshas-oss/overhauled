#include "adi_server.h"

#include <csignal>
#include <cstdlib>
#include <iostream>

using namespace nvlink;

namespace {

nvlink::adi::AdiServer* g_server = nullptr;

void handle_signal(int) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [port] [backlog-threshold]\n";
    std::cout << "Starts the ADI binary protocol server with NVLink-aware task placement.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }

    try {
        int port = (argc > 1) ? std::atoi(argv[1]) : 8080;
        std::size_t backlog_threshold = (argc > 2) ? static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10)) : 32;

        GpuTopology topology = GpuTopology::detect();
        topology.print_info();
        topology.enable_peer_access();

        nvlink::adi::AdiServerConfig config;
        config.port = port;
        config.backlog_threshold = backlog_threshold;
        config.verbose = true;

        nvlink::adi::AdiServer server(topology, config);
        g_server = &server;
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        std::cout << "Starting ADI server on port " << port << " with backlog threshold "
                  << backlog_threshold << "\n";
        std::cout << "Press Ctrl+C to stop the server.\n";
        server.run();
        std::cout << "ADI server stopped cleanly.\n";
        return 0;
    } catch (const NVLinkError& ex) {
        std::cerr << "NVLink error: " << ex.what() << "\n";
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
