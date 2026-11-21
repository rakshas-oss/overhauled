# Overhauled versioning

## Current release candidate

- **Proposed version:** `1.5.1`
- **CMake project version:** `1.5.1`
- **Conan recipe version:** `1.5.1`
- **vcpkg manifest version:** `1.5.1`
- **Runtime/package identity:** `OverhauledTensorRuntime` / `nvlink_placement`
- **Release state:** patch candidate; v1.5.0 is published; v1.5.1 tag and release archive are pending

## Version assessment

The v1.5.1 patch follows published v1.5.0 and corrects the optional Wasmtime v51 C API integration and its regression test:

- Use the supported `wasm_name_t` lifecycle (`wasm_name_new_empty` / `wasm_name_delete`) for trap messages; remove calls to unavailable `wasm_message_new_empty`, `wasm_message_delete`, and `wasmtime_val_delete` symbols.
- Validate incompatible consumer imports at module registration, where the Wasmtime linker reports signature mismatches, rather than expecting instantiation to fail.
- Preserve the optional `ENABLE_WASM_RUNTIME` feature and its existing runtime-composition API.
- Keep the default build unchanged when `ENABLE_WASM_RUNTIME=OFF`; no BRK1, BRK2 or WSM1 wire-format change is intended.

This patch does not expand the sandbox security model. Modules still share one Store/resource domain; fuel metering, epoch interruption, WASI capability grants, artifact signing/cache eviction and task-level teardown remain incomplete.

## Verified CI result (2026-11-21)

The documentation update merged as commit `bacb67ec5918697ffabd0e533b1c403f85105a1a`. All **8/8 GitHub Actions checks** on that merge commit completed successfully:

- Paired repository release gate.
- Wasmtime v51 compile and runtime composition.
- BRK2 C++ codec and mutation tests.
- Paired YuKKi-OS BRK1/BRK2 interoperability.
- ASan + UBSan validation and the Overhauled C++ sanitizer verification.
- Installed CMake package consumer tests on Ubuntu and Windows.

This is a successful CI result for the documentation merge commit and the code it contains; it is not by itself a v1.5.1 release approval. The Windows result is an installed-package consumer test, not a claim that native Windows/MSVC broker execution is supported. The Wasmtime lane remains the documented Linux x86_64 / Wasmtime C API v51.0.0 target.

## Release policy

- v1.5.0 is the published baseline. Its Wasmtime-enabled build was affected by the C API mismatch described above.
- The v1.5.1 release-metadata PR is merged and the corrected mainline CI gates listed above pass. Still verify the exact intended release/tag commit and release artifacts before tagging or publishing v1.5.1.
- Updating this repository does not publish Conan Center, vcpkg, Homebrew, apt or dnf packages. Verify each external registry separately.
- Update vcpkg/Homebrew archive URLs and checksums only from the exact final v1.5.1 release archive.
- YuKKi-OS owns workload semantics, spatial resolution and directives. Overhauled owns broker queues, admission, local physical CPU/NUMA worker placement, GPU topology, accelerator placement and execution.
- The locality codec is a wire contract only: no broker-owned payload allocator applies or verifies physical memory placement. Do not claim locality for WASM linear memory, datasets, accelerator staging, GPU VRAM or NPU memory.
- Deadlines order and reject expired queued work; they do not preempt arbitrary native code or provide a hard real-time guarantee.
- Mainline Windows/MSVC is unsupported. Browser-WASM and WASI are not Overhauled deployment targets.

## Release candidate history

- `v1.4.0` — merged release candidate for the BRK2 locality-placement codec; release tag/publication remains subject to its release gates.

## Published release history

- `v1.3.0` — published 2026-11-21; hierarchical worker topology, per-class CPU-group queues, bounded elasticity and large-host scheduling.
- `v1.2.0` — published 2026-11-20; YuKKi-aware worker classes, broker-local NUMA affinity, process-wide admission budgets and BRK2 worker telemetry.
- `v1.1.4` — published 2026-11-18; architecture/heuristic and release metadata alignment.
- `v1.1.3` — published 2026-11-18; release metadata and prior broker hardening.
- `v1.1.2` — prior published release.
