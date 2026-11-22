# Overhauled versioning

## Current release candidate

- **Proposed version:** `1.6.0`
- **CMake project version:** `1.6.0`
- **Conan recipe version:** `1.6.0`
- **vcpkg manifest version:** `1.6.0`
- **Runtime/package identity:** `OverhauledTensorRuntime` / `nvlink_placement`
- **Release state:** candidate only; v1.5.0 remains the published baseline. Do not describe v1.6.0 as released until the exact approved commit is tagged and its release assets are verified.

## Version assessment

The v1.6.0 minor candidate carries additive broker-side WASM handoff receiver primitives on top of the Wasmtime v51 correction previously tracked as v1.5.1:

- OpenSSL EVP Ed25519 verification over caller-supplied RFC 8785 canonical message bytes.
- Operator-provisioned trust entries binding key IDs to exact issuer, subject and audience values.
- SHA-256 digest parsing and hashing.
- Bounded process-local replay guard keyed by sender, audience and nonce.
- Strict sequential artifact chunk staging with per-chunk digest checks and final artifact digest verification.
- Opt-in CMake integration via `ENABLE_WASM_HANDOFF=ON`; OpenSSL Crypto is required only for that feature. The default broker build and the separate `ENABLE_WASM_RUNTIME` Wasmtime option remain unchanged.

This is a meaningful additive capability and warrants a minor version increment. It is not a complete network-facing handoff implementation: there is no authenticated BRK2 handoff endpoint, complete schema decoder/canonicalizer, transport identity binding, publisher-provenance verifier, durable replay store, signed prepare/receipt flow, runtime backend admission, or release activation. Do not expose or advertise a deployable authenticated handoff service yet.

## Compatibility and security boundaries

- No BRK1, BRK2 or WSM1 wire-format changes are introduced by the receiver primitives.
- The verifier consumes already-canonicalized RFC 8785 bytes; it does not canonicalize JSON itself.
- Trust keys must be provisioned out of band and must never be accepted from the incoming artifact.
- Replay protection is process-local and bounded. Multi-process deployments need shared replay state or routing to a single replay authority.
- Artifacts are staged sequentially and bytes are unavailable through the API until final digest verification succeeds.
- `ENABLE_WASM_HANDOFF` and `ENABLE_WASM_RUNTIME` are independent opt-in features.
- The Wasmtime C API embedding still shares one Store/resource domain and is not tenant isolation. Fuel metering, epoch interruption, fine-grained WASI grants and per-task isolation/teardown remain incomplete.
- The locality codec remains a wire contract only; no broker-owned payload allocator applies or verifies physical memory placement.
- Deadlines order and reject expired queued work; they do not preempt arbitrary native code or provide hard real-time guarantees.
- Mainline Windows/MSVC is unsupported. Browser-WASM and WASI are not Overhauled deployment targets.

## Release policy

- v1.5.0 is the published baseline and introduced optional native Wasmtime runtime composition.
- The v1.5.1 patch candidate corrected the Wasmtime v51 C API integration. Its correction is included in the v1.6.0 candidate; do not claim v1.5.1 was published unless a corresponding tag/release is independently verified.
- A successful merge or CI run is not a release. Tag and publish only from the exact approved release commit after all required checks pass.
- Updating CMake, Conan and vcpkg repository metadata does not publish external package-manager packages. Verify each registry independently.
- The Conan and vcpkg default recipes do not enable `ENABLE_WASM_HANDOFF`; consumers building the optional handoff feature directly with CMake must install OpenSSL Crypto and enable the option explicitly.

## Published release history

- `v1.5.0` — published 2026-11-21; optional native Wasmtime runtime composition.
- `v1.3.0` — published 2026-11-21; hierarchical worker topology, per-class CPU-group queues, bounded elasticity and large-host scheduling.
- `v1.2.0` — published 2026-11-20; YuKKi-aware worker classes, broker-local NUMA affinity, process-wide admission budgets and BRK2 worker telemetry.
- `v1.1.4` — published 2026-11-18; architecture/heuristic and release metadata alignment.
- `v1.1.3` — published 2026-11-18; release metadata and prior broker hardening.
- `v1.1.2` — prior published release.
