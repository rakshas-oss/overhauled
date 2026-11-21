// SPDX-License-Identifier: MIT
#include "wasm_composition.h"

#include <stdexcept>
#include <utility>

namespace nvlink::wasm {
namespace {
std::string error_message(wasmtime_error_t* error) {
    if (!error) return {};
    wasm_name_t message;
    wasm_name_new_empty(&message);
    wasmtime_error_message(error, &message);
    std::string result(message.data, message.size);
    wasm_name_delete(&message);
    wasmtime_error_delete(error);
    return result;
}

std::string trap_message(wasm_trap_t* trap) {
    if (!trap) return {};
    // The Wasmtime v51 C API uses wasm_name_t for trap messages. The
    // wasm_message_new_empty/delete helpers are not part of this API.
    wasm_name_t message;
    wasm_name_new_empty(&message);
    wasm_trap_message(trap, &message);
    std::string result(message.data, message.size);
    wasm_name_delete(&message);
    wasm_trap_delete(trap);
    return result;
}
} // namespace

void WasmCompositionRuntime::EngineDeleter::operator()(wasm_engine_t* p) const { if (p) wasm_engine_delete(p); }
void WasmCompositionRuntime::StoreDeleter::operator()(wasmtime_store_t* p) const { if (p) wasmtime_store_delete(p); }
void WasmCompositionRuntime::LinkerDeleter::operator()(wasmtime_linker_t* p) const { if (p) wasmtime_linker_delete(p); }
void WasmCompositionRuntime::ModuleDeleter::operator()(wasmtime_module_t* p) const { if (p) wasmtime_module_delete(p); }

WasmCompositionRuntime::WasmCompositionRuntime() : WasmCompositionRuntime(Limits{}) {}

WasmCompositionRuntime::WasmCompositionRuntime(Limits limits)
    : engine_(wasm_engine_new()),
      store_(engine_ ? wasmtime_store_new(engine_.get(), nullptr, nullptr) : nullptr),
      linker_(engine_ ? wasmtime_linker_new(engine_.get()) : nullptr),
      max_module_bytes_(limits.max_module_bytes) {
    if (!engine_ || !store_ || !linker_) {
        throw std::runtime_error("failed to initialize Wasmtime runtime");
    }
    wasmtime_store_limiter(store_.get(), limits.max_memory_bytes,
                           limits.max_table_elements, limits.max_instances,
                           limits.max_tables, limits.max_memories);
}

WasmCompositionRuntime::~WasmCompositionRuntime() = default;

bool WasmCompositionRuntime::compile_and_register(
    const std::string& module_name, const std::vector<std::uint8_t>& wasm,
    std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (module_name.empty()) return fail("module name must not be empty");
    if (modules_.count(module_name)) return fail("module namespace already registered");
    if (wasm.empty() || wasm.size() > max_module_bytes_)
        return fail("module binary is empty or exceeds configured byte limit");

    wasmtime_module_t* raw = nullptr;
    if (auto* compile_error = wasmtime_module_new(engine_.get(), wasm.data(), wasm.size(), &raw))
        return fail(error_message(compile_error));
    ModulePtr module(raw);

    if (auto* link_error = wasmtime_linker_module(
            linker_.get(), wasmtime_store_context(store_.get()),
            module_name.data(), module_name.size(), module.get())) {
        return fail(error_message(link_error));
    }
    modules_.emplace(module_name, std::move(module));
    if (error) error->clear();
    return true;
}

bool WasmCompositionRuntime::instantiate(const std::string& module_name,
                                         std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    const auto found = modules_.find(module_name);
    if (found == modules_.end()) return fail("root module is not registered");
    if (instances_.count(module_name)) return fail("module is already instantiated");

    wasmtime_instance_t instance;
    wasm_trap_t* trap = nullptr;
    if (auto* link_error = wasmtime_linker_instantiate(
            linker_.get(), wasmtime_store_context(store_.get()),
            found->second.get(), &instance, &trap)) {
        return fail(error_message(link_error));
    }
    if (trap) return fail(trap_message(trap));
    instances_.emplace(module_name, instance);
    if (error) error->clear();
    return true;
}

bool WasmCompositionRuntime::invoke_i32(
    const std::string& module_name, const std::string& export_name,
    std::int32_t* result, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (!result) return fail("result pointer must not be null");
    const auto found = instances_.find(module_name);
    if (found == instances_.end()) return fail("module is not instantiated");

    auto* context = wasmtime_store_context(store_.get());
    wasmtime_extern_t item;
    if (!wasmtime_instance_export_get(context, &found->second,
                                      export_name.data(), export_name.size(), &item)) {
        return fail("export not found: " + export_name);
    }
    if (item.kind != WASMTIME_EXTERN_FUNC) {
        wasmtime_extern_delete(&item);
        return fail("export is not a function: " + export_name);
    }

    // This ABI returns only an i32, which is an immediate scalar in
    // wasmtime_val_t and requires no value-root cleanup function.
    wasmtime_val_t value{};
    wasm_trap_t* trap = nullptr;
    auto* call_error = wasmtime_func_call(context, &item.of.func,
                                          nullptr, 0, &value, 1, &trap);
    wasmtime_extern_delete(&item);
    if (call_error) return fail(error_message(call_error));
    if (trap) return fail(trap_message(trap));
    if (value.kind != WASMTIME_I32) {
        return fail("export result is not i32: " + export_name);
    }
    *result = value.of.i32;
    if (error) error->clear();
    return true;
}

bool WasmCompositionRuntime::contains(const std::string& module_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return modules_.find(module_name) != modules_.end();
}

} // namespace nvlink::wasm
