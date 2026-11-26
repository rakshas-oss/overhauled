# Overhauled versioning

## Release state

- **Latest published GitHub release:** v1.9.0 (2026-11-26).
- **Next minor release candidate:** v1.10.0 (not tagged or published).
- **CMake project version:** 1.10.0 in this candidate.
- **Conan recipe version:** 1.10.0 in this candidate.
- **Root vcpkg manifest:** 1.10.0 in this candidate.
- **Published vcpkg registry baseline and Homebrew formula:** remain pinned to the last verified immutable source/archive baseline (1.6.0); no 1.10.0 external registry or formula update is claimed.
- **Package identity:** OverhauledTensorRuntime / nvlink_placement.
- **External package uploads:** no Conan remote upload, vcpkg registry publication, Homebrew update, Debian/RPM repository or other package-manager publication is implied by GitHub release metadata.

The v1.10.0 version metadata is a candidate only. Do not create a tag or publish archives until the release gate below passes on the exact candidate commit.

## Version assessment

The published v1.9.0 release added bounded broker client sessions, strict loopback binding for mTLS-proxy deployments, a TLS 1.3 sidecar profile and optional fsync-backed replay-nonce journaling.

The additive public C++ crawler-metrics library merged in Overhauled PR #95 justifies advancing the project version to v1.10.0:

- Add the standalone `crawler_metrics` CMake library and `Overhauled::crawler_metrics` alias target.
- Install the public `crawler_metrics.h` header and export the library through the existing CMake package installation.
- Aggregate page success/failure counts, document bytes, link totals, fetch/extraction duration count/min/max/mean/population standard deviation, and bounded P50/P95/P99 histogram upper bounds.
- Support one fixed-memory accumulator per worker or NUMA shard and merge at a barrier without a shared hot lock.
- Reject integer overflow and self-merge without mutating the accumulator.
- Pair with YuKKi-OS v6.21.0 crawler reports through caller-side normalized `PageObservation` mapping.

This is an embeddable CPU-side reduction library. It does not fetch URLs, parse HTML, receive raw page bodies, define a BRK1/BRK2 message, wire itself automatically to YuKKi-OS, or dispatch metrics reductions to GPU/NPU. Histogram percentile values are bucket upper bounds, not exact sample quantiles.

## Compatibility and security boundaries

- Existing BRK1, BRK2, WSM1, NXR1 and MED1 identities are preserved; v1.10.0 does not add a wire operation.
- The crawler metrics API accepts numeric observations only. Keep raw HTML and network fetching in the YuKKi-OS host-side crawler boundary.
- Keep one accumulator per worker/NUMA shard and merge at a controlled barrier; the accumulator is not internally synchronized.
- Application broker-session, replay-journal and rate/memory limits do not replace upstream network-edge controls.
- The ordinary broker listener is still separate from the authenticated WASM handoff receiver API. The stunnel sidecar is external TLS termination, not native TLS, and its certificate identity is not yet bound to signed sender authorization.
- The optional replay journal persists accepted nonces, not staged artifacts, prepared deployments or active runtime state. The current session I/O architecture remains thread-per-client within its configured ceiling.
- The optional Wasmtime C API composition runtime shares a Store/resource domain; it is not a multi-tenant sandbox.
- CUDA, TensorRT, Ethos-U85 and NVLink hardware features remain separately gated. A CPU-only metrics build does not validate physical accelerator hardware.
- The external vcpkg registry and Homebrew formula must remain pinned to the last archive whose exact checksums and downstream validation are available; changing the root vcpkg manifest does not publish or register a new port.

## Release gate for 1.10.0

Before tagging or publishing:

1. Run the complete Overhauled CI suite on the exact candidate: default CPU-only build/tests, Crawler Metrics CTest, OpenSSL-enabled handoff tests, Wasmtime compile/runtime composition, paired YuKKi interoperability, BRK2 conformance, sanitizers and supported package-consumer smoke tests.
2. Verify the public CMake target and installed header with a downstream consumer; verify the Conan recipe using `conan create . --build=missing`.
3. Confirm aggregation correctness for success/failure status, byte/link totals, zero and boundary durations, fixed histogram bounds, worker-shard merging, self-merge rejection and integer overflow without partial mutation.
4. Verify that no raw HTML or page body is accepted by the metrics library and that no new BRK operation, implicit crawler-to-broker route, GPU/NPU crawler dispatch or performance claim is introduced.
5. Run paired YuKKi-OS/Overhauled interoperability checks and preserve BRK1/BRK2/WSM1/NXR1/MED1 compatibility, bounded session admission, strict mTLS-proxy listener policy and replay-journal restart/replay/corruption/capacity behavior.
6. Only after all required checks pass and the release commit is approved, create the v1.10.0 tag and GitHub Release. Then calculate exact immutable source archive SHA256/SHA512 values and submit separate updates for the Homebrew formula and vcpkg registry. Verify Conan remote and other external package publication independently.

## Published release history

- v1.9.0 — published 2026-11-26; bounded broker sessions, strict mTLS-proxy loopback binding and optional durable replay-nonce journal.
- v1.8.0 — published 2026-11-24; explicit BRK2 GPU preference for bounded workloads while preserving forced-CPU/exact-CPU constraints.
- v1.7.0 — published 2026-11-22; authenticated WASM receiver state machine, bounded admission and stream-flood controls.
- v1.6.0 — published 2026-11-22; opt-in authenticated-WASM receiver primitives and Wasmtime v51 C API correction. The receiver's full state machine, signed decisions/receipts, runtime bridge and bounded rate controls were added after that release by PR #86.
- v1.5.0 — published 2026-11-21; optional native Wasmtime runtime composition.
- v1.3.0 — published 2026-11-21; hierarchical worker topology, per-class CPU-group queues, bounded elasticity and large-host scheduling.
- v1.2.0 — published 2026-11-20; YuKKi-aware worker classes, broker-local NUMA affinity, process-wide admission budgets and BRK2 worker telemetry.
- v1.1.4 — published 2026-11-18; architecture/heuristic and release metadata alignment.
- v1.1.3 — published 2026-11-18; release metadata and prior broker hardening.
- v1.1.2 — prior published release.
