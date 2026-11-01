// SPDX-License-Identifier: MIT
// NPU device support for the overhauled broker.
//
// Gap this closes: BackendKind::Npu and NpuExecutor already existed in
// backend_dispatch.h, but no concrete executor shipped, so
// BrokerService could never select the NPU in practice, and
// --npu-enabled in broker_server_main wired no executor at all.
//
// This file adds two things, deliberately kept separate:
//  * NpuDevice: a plain descriptor for a real NPU device (name/model).
//    A real Ethos-U85 (or other) executor should implement NpuExecutor
//    against its own driver and register itself with this descriptor.
//  * SimulatedNpuExecutor: a CPU-emulated executor for conformance and
//    CI only. It is NOT hardware acceleration and must never be
//    presented as such. It exists so the full YuKKi-OS -> BRK2 -> broker
//    -> NPU -> response path can be tested without NPU hardware, using
//    the same toy double-vector kernel shape as the existing CPU/GPU
//    paths (CPU x2.0, test FakeNpu x3.0). Enable it only with the
//    explicit --npu-simulated broker flag.

#pragma once

#include "backend_dispatch.h"

#include <string>
#include <vector>

namespace nvlink::broker {

struct NpuDevice {
    std::string name = "simulated-npu";
    std::string model = "simulated";
    // True only for SimulatedNpuExecutor. Real devices set this false.
    bool simulated = true;
    bool available = true;
};

class SimulatedNpuExecutor : public NpuExecutor {
public:
    explicit SimulatedNpuExecutor(bool enabled = true) : enabled_(enabled) {}

    bool available() const override { return enabled_; }

    // CPU emulation of the broker's toy kernel, for conformance only.
    std::vector<double> run(const std::vector<double>& input) override {
        std::vector<double> out = input;
        for (double& v : out) {
            v *= 3.0;
        }
        return out;
    }

private:
    bool enabled_;
};

}  // namespace nvlink::broker
