# Overhauled versioning

## Release state

- **Latest published GitHub release:** v1.10.0 (2026-11-26).
- **Next minor release candidate:** v1.11.0 (not tagged or published).
- **CMake project version:** 1.11.0 in this candidate.
- **Conan recipe version:** 1.11.0 in this candidate.
- **Root vcpkg manifest:** 1.11.0 in this candidate.
- **Published vcpkg registry baseline and Homebrew formula:** remain pinned to the last verified immutable source/archive baseline (1.6.0); no 1.11.0 registry port or formula update is included.
- **Package identity:** OverhauledTensorRuntime / nvlink_placement.
- **External package uploads:** no Conan remote upload, vcpkg registry publication, Homebrew update, Debian/RPM repository or other package-manager publication is implied by GitHub release metadata.

The v1.11.0 metadata is a candidate only. Do not create a tag, publish archives, or upload a package until the release gate below passes on the exact candidate commit.

## Version assessment

The published v1.10.0 release added the standalone C++17 crawler_metrics library and installed CMake target for aggregating normalized crawler PageObservation records. Its scope is limited to caller-provided numeric page summaries; it does not fetch URLs, parse HTML or add broker routes.

The additive public C++ multivariate_stats library justifies advancing the project to v1.11.0:

- Add the standalone CMake target multivariate_stats and public header multivariate_stats.h, exported as Overhauled::multivariate_stats.
- Support online pairwise covariance and Pearson correlation for up to 32 dimensions with fixed-size state.
- Preserve missing-value semantics: each dimension pair uses only observations where both dimensions are present; missing values are never coerced to zero.
- Expose pairwise sample counts alongside each covariance/correlation value because different cells can be based on different numbers of observations.
- Support per-worker/per-NUMA-shard accumulators and controlled barrier merges without a shared hot lock.
- Reject invalid dimension counts, invalid presence masks, non-finite present values and overflow without partially mutating the accumulator.

This is a CPU-only numeric reduction library. It does not ingest network streams, authenticate a source, parse crawler content, open sockets, define a BRK1/BRK2 operation, automatically connect to YuKKi-OS, or dispatch aggregation to GPU/NPU. Callers must preserve source identity, sequence and timestamp where needed and must enforce tenant admission, bounded queues and backpressure before forwarding normalized observations.

## Compatibility and security boundaries

- Existing BRK1, BRK2, WSM1, NXR1 and MED1 identities are preserved; v1.11.0 does not add a wire operation.
- The multivariate_stats API accepts normalized numeric observations only. It does not handle raw page bodies or establish provenance or authorization.
- Missing values are omitted per dimension pair. Pairwise sample counts must accompany matrix values; do not treat cells as if they share one common sample count.
- Keep one accumulator per worker/NUMA shard and merge at a controlled barrier; the accumulator is not internally synchronized.
- Application broker-session, replay-journal and rate/memory limits do not replace upstream network-edge controls.
- The ordinary broker listener is still separate from the authenticated WASM handoff receiver API. The stunnel sidecar is external TLS termination, not native TLS, and its certificate identity is not yet bound to signed sender authorization.
- The optional replay journal persists accepted nonces, not staged artifacts, prepared deployments or active runtime state. The current session I/O architecture remains thread-per-client within its configured ceiling.
- The optional Wasmtime C API composition runtime shares a Store/resource domain; it is not a multi-tenant sandbox.
- CUDA, TensorRT, Ethos-U85 and NVLink hardware features remain separately gated. A CPU-only multivariate statistics build does not validate physical accelerator hardware.
- The external vcpkg registry and Homebrew formula must remain pinned to the last archive whose exact checksums and downstream validation are available; changing the root vcpkg manifest does not publish or register a new port.

## Release gate for 1.11.0

Before tagging or publishing:

1. Run the full Overhauled CI suite on the exact candidate: CPU-only CMake/CTest, multivariate_stats_test, crawler_metrics_test, OpenSSL-enabled handoff tests, Wasmtime compile/runtime composition, paired YuKKi interoperability, BRK2 conformance, ASan/UBSan and supported package-consumer smoke tests.
2. Verify installed CMake package exports both Overhauled::crawler_metrics and Overhauled::multivariate_stats, including their installed public headers; verify the Conan recipe using conan create . --build=missing.
3. Confirm multivariate correctness for complete and missing dimensions, unequal pair counts, zero variance, insufficient samples, maximum dimension count, shard merge, invalid masks and overflow rollback.
4. Verify that no raw HTML or page body is accepted by multivariate_stats and that no new BRK operation, implicit observation-to-broker route, GPU/NPU aggregation dispatch or performance claim is introduced.
5. Preserve BRK1/BRK2/WSM1/NXR1/MED1 compatibility, bounded session admission, strict mTLS-proxy listener policy and replay-journal restart/replay/corruption/capacity behavior.
6. Only after all required checks pass and the release commit is approved, create the v1.11.0 tag and GitHub Release. Then calculate exact immutable source archive SHA256/SHA512 values and submit separate updates for the Homebrew formula and vcpkg registry. Verify Conan remote and other external package publication independently.

## Published release history

- v1.10.0 — published 2026-11-26; constant-memory CPU crawler-metrics library and installed CMake target.
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
