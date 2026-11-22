// SPDX-License-Identifier: MIT
#include "wasm_handoff_runtime_bridge.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' << __LINE__ << "\n"; \
        std::exit(EXIT_FAILURE); \
    } \
} while (false)

int main() {
    using namespace nvlink::broker::wasm_handoff;

    const std::vector<std::uint8_t> provider = {
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
        0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
        0x03,0x02,0x01,0x00,
        0x07,0x07,0x01,0x03,0x72,0x75,0x6e,0x00,0x00,
        0x0a,0x06,0x01,0x04,0x00,0x41,0x2a,0x0b
    };

    nvlink::wasm::WasmCompositionRuntime::Limits limits;
    WasmHandoffRuntimeBridge bridge(limits, "test-wasmtime-version");
    HandoffPolicy policy;
    bridge.bind_policy(policy, true);

    CHECK(policy.wasmtime_available);
    CHECK(policy.allow_trusted_shared_store_workloads);
    CHECK(policy.engine == "wasmtime");
    CHECK(policy.engine_version == "test-wasmtime-version");
    CHECK(static_cast<bool>(policy.module_validator));
    CHECK(static_cast<bool>(policy.activate_release));

    ModuleDescriptor descriptor;
    descriptor.module_id = "provider";
    descriptor.version = "1.0.0";
    descriptor.size_bytes = provider.size();
    descriptor.format = "wasm-core-1";
    std::string error;
    CHECK(policy.module_validator(descriptor, provider, &error));

    const std::vector<VerifiedModuleView> views = {
        VerifiedModuleView{&descriptor, &provider}
    };
    CHECK(policy.activate_release("deployment-1", "release-1", views, &error));
    const auto release_one = bridge.active_runtime();
    CHECK(release_one != nullptr);
    CHECK(release_one->contains("provider"));
    CHECK(bridge.active_release() == std::make_pair(
        std::string("deployment-1"), std::string("release-1")));

    // A bad staged module must not replace the active runtime pointer.
    const std::vector<std::uint8_t> invalid = {0x00, 0x01, 0x02, 0x03};
    ModuleDescriptor invalid_descriptor = descriptor;
    invalid_descriptor.module_id = "invalid";
    invalid_descriptor.size_bytes = invalid.size();
    const std::vector<VerifiedModuleView> invalid_views = {
        VerifiedModuleView{&invalid_descriptor, &invalid}
    };
    CHECK(!policy.activate_release("deployment-1", "release-bad", invalid_views, &error));
    CHECK(bridge.active_runtime() == release_one);
    CHECK(bridge.active_release().second == "release-1");

    // Promotion stages an entirely new runtime. Existing readers may retain
    // the previous shared_ptr while new readers observe the new release.
    CHECK(policy.activate_release("deployment-1", "release-2", views, &error));
    const auto release_two = bridge.active_runtime();
    CHECK(release_two != nullptr);
    CHECK(release_two != release_one);
    CHECK(release_one->contains("provider"));
    CHECK(release_two->contains("provider"));
    CHECK(bridge.active_release().second == "release-2");

    std::cout << "WASM handoff runtime bridge test passed\n";
    return EXIT_SUCCESS;
}
