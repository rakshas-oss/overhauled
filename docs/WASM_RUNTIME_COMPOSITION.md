# Native Wasmtime runtime composition

Overhauled has an **optional native Wasmtime C API integration** for deployments
that need to compile and compose core-Wasm modules inside the broker process.
This is separate from the WSM1 task/lifecycle protocol. It does not change the
default broker build, and it does not move YuKKi-OS guest execution into the
broker's WSM1 path.

## Supported build configuration

The integration is pinned and exercised in CI against **Wasmtime C API v51.0.0**
on Linux x86_64. Other Wasmtime versions and target platforms are not implied
to be supported merely because their headers or libraries are discoverable.

The Wasmtime C API development artifact must provide:

- `include/wasmtime.h` (and its transitive C API headers);
- a linkable `libwasmtime` in `lib` or `lib64`;
- the runtime shared-library dependencies needed by the target platform.

Set `WASMTIME_ROOT` to the extracted/install prefix containing those
`include` and library directories. Example:

```sh
cmake -S . -B build-wasmtime \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BROKER=ON \
  -DBUILD_EXAMPLES=OFF \
  -DENABLE_CUDA=OFF \
  -DENABLE_TENSORRT=OFF \
  -DENABLE_WASM_RUNTIME=ON \
  -DWASMTIME_ROOT=/opt/wasmtime
cmake --build build-wasmtime --parallel
ctest --test-dir build-wasmtime --output-on-failure
```

Use a separate build directory for this optional dependency so a CPU-only
baseline remains available. If CMake cannot find both the header and library,
configuration fails rather than silently disabling the requested runtime.

The default remains `ENABLE_WASM_RUNTIME=OFF`. With that option disabled,
Overhauled does not compile or link `src/wasm_composition.cpp`, and no
Wasmtime installation is required.

## Wasmtime v51 API and regression coverage

The supported CI lane pins Wasmtime C API **v51.0.0**. In this API, trap-message
handling uses `wasm_name_t` and its `wasm_name_new_empty` /
`wasm_name_delete` lifecycle. Do not use the nonexistent
`wasm_message_new_empty` or `wasm_message_delete` symbols. Scalar
`wasmtime_val_t` results are immediate values; do not pass them to the
nonexistent `wasmtime_val_delete` cleanup function.

The composition test covers:

1. compilation and registration of a provider module;
2. compilation and registration of a consumer with typed imports from the
   provider namespace;
3. instantiation and invocation of the consumer's exported `() -> i32`
   function, verifying the value returned through the imported provider;
4. rejection of an incompatible import signature **during consumer
   registration**. Wasmtime's linker validates import compatibility while
   defining/registering the module, before instantiation.

Run the full CTest suite in the Wasmtime-enabled build. A successful default
CPU build does not exercise this optional code path. Conversely, a successful
Wasmtime compile alone is not enough: the provider/consumer runtime test must
also pass.

## Composition model and API boundary

`WasmCompositionRuntime::compile_and_register(name, bytes, ...)` validates
and compiles a module, then registers it in the Wasmtime linker under `name`.
Register provider namespaces before dependent modules. The linker resolves
import names and types; this is runtime composition, not static binary merging
or rewriting.

The runtime retains the instantiated root handle. The initial invocation API
is deliberately narrow: exported functions with signature `() -> i32`, called
through `invoke_i32(module, export, &result, ...)`. Missing exports,
non-function exports, traps and incompatible result types are errors; values
are not coerced.

## Isolation, security and resource limits

The runtime owns one Wasmtime engine, store and linker. Registered modules and
retained instances share that Store and therefore share one resource and
isolation domain. Store-level linear-memory, table, instance and memory-count
limits constrain resources, but they do **not** create per-module or
per-tenant isolation.

Repeated instantiation of the same registered root name is rejected by this
initial API. Instance teardown/replacement is not yet exposed. Fuel metering,
epoch interruption, WASI capability grants, artifact persistence and signature
verification, cache eviction, and task-level instance teardown are not
implicitly enabled by this embedding layer. Do not expose it as a complete
untrusted-code sandbox. Where trust domains differ, use separate runtime
instances/processes and an independently designed capability and lifecycle
policy; process separation is the stronger boundary.

## Relationship to YuKKi-OS and WSM1

There are two distinct execution paths:

- **YuKKi-OS host path (existing control-plane integration):** YuKKi-OS owns
  guest validation, Wasmtime compilation, instantiation, guest memory and host
  imports. WSM1 carries host-prepared task data and module-version lifecycle
  metadata to Overhauled. WSM1 does not upload executable Wasm modules.
- **Overhauled native composition path (optional):** an application that
  explicitly enables `ENABLE_WASM_RUNTIME` can use the native
  `WasmCompositionRuntime` API to compile/register/compose core-Wasm modules
  locally in Overhauled. This is not an automatic handoff from YuKKi-OS, is not
  activated by WSM1, and does not replace the YuKKi-OS host runtime.

Choose one owner for a guest execution lifecycle. Do not instantiate the same
guest independently in both processes and assume shared state, shared memory,
or coordinated activation. Cross-process state and lifecycle coordination must
be implemented explicitly by the application.

The existing BRK1/BRK2/WSM1 wire identities and fields are unchanged by this
optional C API integration. See [WASM interoperability](WASM_INTEROP.md) for
the WSM1 wire contract and [YuKKi-OS composition guide](https://github.com/rakshas-oss/YuKKi-OS/blob/main/docs/WASM_PROGRAM_COMPOSITION.md)
for the YuKKi-OS host-side composition path.
