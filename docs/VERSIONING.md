# Overhauled versioning

## Current release candidate

- **Proposed version:** `1.5.0`
- **CMake project version:** `1.5.0`
- **Conan recipe version:** `1.5.0`
- **vcpkg manifest version:** `1.5.0`
- **Runtime/package identity:** `OverhauledTensorRuntime` / `nvlink_placement`
- **Release state:** minor candidate; v1.5.0 tag and release archive are pending

## Version assessment

The v1.5.1 patch follows published v1.5.0 and repairs the opt-in Wasmtime v51 runtime-composition build and test:

- Optional `ENABLE_WASM_RUNTIME` embeds the Wasmtime C API to compile core-Wasm binaries and register modules under linker namespaces.
- Dependent modules resolve typed imports at instantiation; runtime composition does not merge or rewrite module binaries.
- Retained instances expose an initial `() -> i32` invocation ABI with explicit missing-export, trap and result-type errors.
- The default build remains unchanged when the option is OFF.

This is an additive runtime capability, not a complete multi-tenant sandbox. Modules share one Store/resource domain; fuel metering, epoch interruption, WASI capability grants, artifact signing/cache eviction and task-level teardown remain incomplete. Existing BRK1/BRK2/WSM1 envelope identities are unchanged.

## Release policy

- v1.3.0 is the latest published Overhauled release (2026-11-21).
- Do not create or publish v1.5.0 until its release PR is reviewed and required
  C++ builds/tests, BRK2 conformance and mutation tests, sanitizers,
  package-consumer tests, and live cross-repository interoperability checks
  pass on the exact release candidate commit.
- Updating this repository does not publish Conan Center, vcpkg, Homebrew,
  apt or dnf packages. Verify each external registry separately.
- The root vcpkg manifest is set to candidate 1.5.1; update the vcpkg portfile
  source reference and SHA512 only from the exact final tagged archive.
- The checked-in Homebrew formula still points to v1.1.1. Refresh its URL,
  version and SHA-256 only from the final v1.5.1 release archive.
- YuKKi-OS owns workload semantics, spatial resolution and directives.
  Overhauled owns broker queues, admission, local physical CPU/NUMA worker
  placement, GPU topology, accelerator placement and execution.
- The locality codec is a wire contract only: no broker-owned payload
  allocator applies or verifies physical memory placement. Do not claim
  locality for WASM linear memory, datasets, accelerator staging, GPU VRAM or
  NPU memory.
- Deadlines order and reject expired queued work; they do not preempt arbitrary
  native code or provide a hard real-time guarantee.
- Mainline Windows/MSVC is unsupported. Browser-WASM and WASI are not
  Overhauled deployment targets.

## Release candidate history

- `v1.4.0` — merged release candidate for the BRK2 locality-placement codec; release tag/publication remains subject to its release gates.

## Published release history

- `v1.3.0` — published 2026-11-21; hierarchical worker topology, per-class
  CPU-group queues, bounded elasticity and large-host scheduling.
- `v1.2.0` — published 2026-11-20; YuKKi-aware worker classes, broker-local
  NUMA affinity, process-wide admission budgets and BRK2 worker telemetry.
- `v1.1.4` — published 2026-11-18; architecture/heuristic and release
  metadata alignment.
- `v1.1.3` — published 2026-11-18; release metadata and prior broker hardening.
- `v1.1.2` — prior published release.
