# Wasm runtime composition

Overhauled can optionally embed Wasmtime to compile core WebAssembly binaries and
compose independently compiled modules at runtime. This is runtime composition,
not static binary merging.

## Build

Install the Wasmtime C API development artifact for the target platform, then:

```sh
cmake -S . -B build -DENABLE_WASM_RUNTIME=ON -DWASMTIME_ROOT=/path/to/wasmtime
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The Wasmtime C API installation must provide `include/wasmtime.h` and the
`wasmtime` library under its include/lib directories. The default build remains
unchanged when the option is OFF.

## Composition model

`WasmCompositionRuntime::compile_and_register(name, bytes, ...)` validates and
compiles a module, then registers it in the Wasmtime linker under `name`.
A dependent module can declare imports from that namespace and call its exports.
Register dependencies before registering/instantiating dependants. Instantiate
the selected root module only after its dependency namespaces have been added.

Modules remain separate compiled artifacts and retain their own versions. The
linker resolves import names and types; no static module merge or rewriting occurs.
A missing import, incompatible import type, invalid binary, duplicate namespace,
or trapped start function is reported as an error.

## Isolation and current boundaries

The runtime owns one Wasmtime engine, store and linker. The store applies configured
linear-memory, table, instance and memory-count limits. The default memory ceiling
is 256 MiB per memory, with bounded table/instance counts. Production callers should
create a runtime per tenant or trust domain where isolation boundaries differ.
Fuel metering, epoch interruption, WASI capability grants, artifact persistence,
signature verification, cache eviction and task-level instance teardown are not
implicitly enabled by this initial embedding layer and must be configured before
exposing untrusted workloads. Do not treat this API as a complete multi-tenant
sandbox policy by itself.

The existing WSM1 broker protocol remains the host-to-broker task/lifecycle
transport. It does not transmit compiled machine code or replace module-level
authorization. YuKKi-OS may continue to own its host-side Wasmtime sandbox; this
new Overhauled capability provides an optional native embedding path for broker
deployments that need to compile and compose modules locally.
