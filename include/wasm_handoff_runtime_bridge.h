// SPDX-License-Identifier: MIT
#pragma once

#include "wasm_composition.h"
#include "wasm_handoff_receiver.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace nvlink::broker::wasm_handoff {

/// Connects the authenticated receiver callbacks to the optional native
/// Wasmtime composition runtime. A new runtime/store is staged for each
/// release; only a fully compiled module set is atomically promoted.
class WasmHandoffRuntimeBridge {
public:
    explicit WasmHandoffRuntimeBridge(
        nvlink::wasm::WasmCompositionRuntime::Limits limits,
        std::string engine_version);

    /// Installs actual compile-only validation and staged-release activation
    /// callbacks into a receiver policy. Trusted shared-Store execution must
    /// be explicitly enabled by the operator.
    void bind_policy(HandoffPolicy& policy,
                     bool allow_trusted_shared_store_workloads) const;

    std::shared_ptr<nvlink::wasm::WasmCompositionRuntime> active_runtime() const;
    std::pair<std::string, std::string> active_release() const;

private:
    struct State;
    std::shared_ptr<State> state_;
    std::string engine_version_;
};

} // namespace nvlink::broker::wasm_handoff
