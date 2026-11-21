// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <wasmtime.h>

namespace nvlink::wasm {

/// Runtime composition of independently compiled core Wasm modules.
///
/// Modules are registered under the import namespace used by dependent modules.
/// Wasmtime resolves imports at instantiation time; modules remain independently
/// compiled and versionable. All registered modules and instances share one
/// Store and therefore one isolation/resource-limit domain.
class WasmCompositionRuntime {
public:
    struct Limits {
        std::size_t max_module_bytes = 16U * 1024U * 1024U;
        std::int64_t max_memory_bytes = 256LL * 1024LL * 1024LL;
        std::int64_t max_table_elements = 100000;
        std::int64_t max_instances = 128;
        std::int64_t max_tables = 128;
        std::int64_t max_memories = 128;
    };

    WasmCompositionRuntime();
    explicit WasmCompositionRuntime(Limits limits);
    ~WasmCompositionRuntime();

    WasmCompositionRuntime(const WasmCompositionRuntime&) = delete;
    WasmCompositionRuntime& operator=(const WasmCompositionRuntime&) = delete;

    /// Compile and register a module under its import namespace.
    /// A dependent module can import exports as "<module_name>.<export_name>".
    bool compile_and_register(const std::string& module_name,
                              const std::vector<std::uint8_t>& wasm,
                              std::string* error);

    /// Instantiate a registered root module after resolving its imports through
    /// the linker. Dependencies must already have been registered. Repeated
    /// instantiation under the same module name is rejected.
    bool instantiate(const std::string& module_name, std::string* error);

    /// Invoke an exported function with signature () -> i32 on an instantiated
    /// module. This deliberately narrow ABI avoids implicit host-value coercion.
    bool invoke_i32(const std::string& module_name, const std::string& export_name,
                    std::int32_t* result, std::string* error);

    bool contains(const std::string& module_name) const;

private:
    struct EngineDeleter { void operator()(wasm_engine_t* p) const; };
    struct StoreDeleter { void operator()(wasmtime_store_t* p) const; };
    struct LinkerDeleter { void operator()(wasmtime_linker_t* p) const; };
    struct ModuleDeleter { void operator()(wasmtime_module_t* p) const; };

    using EnginePtr = std::unique_ptr<wasm_engine_t, EngineDeleter>;
    using StorePtr = std::unique_ptr<wasmtime_store_t, StoreDeleter>;
    using LinkerPtr = std::unique_ptr<wasmtime_linker_t, LinkerDeleter>;
    using ModulePtr = std::unique_ptr<wasmtime_module_t, ModuleDeleter>;

    mutable std::mutex mutex_;
    EnginePtr engine_;
    StorePtr store_;
    LinkerPtr linker_;
    std::size_t max_module_bytes_ = 0;
    std::unordered_map<std::string, ModulePtr> modules_;
    std::unordered_map<std::string, wasmtime_instance_t> instances_;
};

} // namespace nvlink::wasm
