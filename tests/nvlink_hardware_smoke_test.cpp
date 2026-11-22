// SPDX-License-Identifier: MIT
// Physical NVLink integration smoke test. This target is opt-in and must only
// run on a machine with at least two CUDA GPUs connected by NVLink.

#include "nvlink_placement.h"

#include <exception>
#include <iostream>

int main() {
    try {
        const auto topology = nvlink::GpuTopology::detect();
        if (topology.num_gpus() < 2) {
            std::cerr << "NVLink hardware validation requires at least two visible CUDA GPUs; found "
                      << topology.num_gpus() << ".\n";
            return 1;
        }

        int nvlink_pairs = 0;
        for (int source = 0; source < topology.num_gpus(); ++source) {
            for (int destination = 0; destination < topology.num_gpus(); ++destination) {
                if (source != destination && topology.has_nvlink(source, destination)) {
                    ++nvlink_pairs;
                }
            }
        }

        if (nvlink_pairs == 0) {
            std::cerr << "CUDA GPUs are visible, but the detected topology contains no NVLink peer pair.\n";
            topology.print_info();
            return 1;
        }

        topology.enable_peer_access();
        topology.print_info();
        std::cout << "nvlink_hardware_smoke_test passed: " << topology.num_gpus()
                  << " GPU(s), " << nvlink_pairs << " directed NVLink peer link(s).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NVLink hardware validation failed: " << error.what() << '\n';
        return 1;
    }
}
