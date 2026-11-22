// SPDX-License-Identifier: MIT
#include "wasm_handoff_runtime_bridge.h"

#include <stdexcept>
#include <utility>

namespace nvlink::broker::wasm_handoff {

struct WasmHandoffRuntimeBridge::State {
    explicit State(nvlink::wasm::WasmCompositionRuntime::Limits value)
        : limits(value), validator(std::make_shared<nvlink::wasm::WasmCompositionRuntime>(value)) {}

    nvlink::wasm::WasmCompositionRuntime::Limits limits;
    std::shared_ptr<nvlink::wasm::WasmCompositionRuntime> validator;
    mutable std::mutex mutex;
    std::shared_ptr<nvlink::wasm::WasmCompositionRuntime> active;
    std::string deployment_id;
    std::string release_id;
};

WasmHandoffRuntimeBridge::WasmHandoffRuntimeBridge(
    nvlink::wasm::WasmCompositionRuntime::Limits limits,
    std::string engine_version)
    : state_(std::make_shared<State>(limits)),
      engine_version_(std::move(engine_version)) {
    if (engine_version_.empty() || engine_version_.size() > 64) {
        throw std::invalid_argument("Wasmtime engine version must be non-empty and bounded");
    }
}

void WasmHandoffRuntimeBridge::bind_policy(
    HandoffPolicy& policy, bool allow_trusted_shared_store_workloads) const {
    const auto state = state_;
    policy.wasmtime_available = true;
    policy.engine = "wasmtime";
    policy.engine_version = engine_version_;
    policy.backend_id = "overhauled-wasmtime-c-api";
    policy.abi_profile = "wasm-core-1";
    policy.allow_trusted_shared_store_workloads = allow_trusted_shared_store_workloads;

    policy.module_validator = [state](
        const ModuleDescriptor& descriptor,
        const std::vector<std::uint8_t>& bytes,
        std::string* error) {
        if (descriptor.format != "wasm-core-1") {
            if (error) *error = "unsupported module format";
            return false;
        }
        return state->validator->validate_module(bytes, error);
    };

    policy.activate_release = [state](
        const std::string& deployment_id,
        const std::string& release_id,
        const std::vector<VerifiedModuleView>& modules,
        std::string* error) {
        auto fail = [&](const std::string& message) {
            if (error) *error = message;
            return false;
        };
        if (deployment_id.empty() || release_id.empty() || modules.empty()) {
            return fail("deployment, release and module set must be non-empty");
        }

        // Compile and register into an unpublished Store/Linker. The old
        // active runtime remains available to readers throughout preparation.
        std::shared_ptr<nvlink::wasm::WasmCompositionRuntime> staged;
        try {
            staged = std::make_shared<nvlink::wasm::WasmCompositionRuntime>(state->limits);
        } catch (const std::exception& exception) {
            return fail(std::string("failed to create staged Wasmtime runtime: ") + exception.what());
        }

        for (const auto& module : modules) {
            if (!module.descriptor || !module.bytes ||
                module.descriptor->format != "wasm-core-1" ||
                module.bytes->empty() ||
                module.bytes->size() != module.descriptor->size_bytes) {
                return fail("verified module view is incomplete or inconsistent");
            }
            if (!staged->compile_and_register(
                    module.descriptor->module_id, *module.bytes, error)) {
                return false;
            }
        }

        // Atomic pointer publication is the runtime promotion point. Any
        // caller already holding the previous shared_ptr can finish using it;
        // new callers see only the newly staged release.
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->active = std::move(staged);
            state->deployment_id = deployment_id;
            state->release_id = release_id;
        }
        if (error) error->clear();
        return true;
    };
}

std::shared_ptr<nvlink::wasm::WasmCompositionRuntime>
WasmHandoffRuntimeBridge::active_runtime() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->active;
}

std::pair<std::string, std::string>
WasmHandoffRuntimeBridge::active_release() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return {state_->deployment_id, state_->release_id};
}

} // namespace nvlink::broker::wasm_handoff
