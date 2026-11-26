# Overhauled versioning

## Release state

- **Latest published version:** 1.8.0 (2026-11-24).
- **Next minor release candidate:** 1.9.0 (not tagged or published).
- **CMake project version:** 1.9.0 in this candidate.
- **Conan recipe version:** 1.9.0 in this candidate.
- **Root vcpkg manifest:** 1.9.0 in this candidate.
- **Published vcpkg registry baseline and Homebrew formula:** remain pinned to the last verified source/archive baseline pending exact source checksums and registry updates.
- **Package identity:** OverhauledTensorRuntime / nvlink_placement.

No 1.9.0 tag, GitHub Release, Homebrew update, vcpkg registry entry, or external package upload is claimed by this candidate.

## Version assessment

The published 1.8.0 release added explicit BRK2 GPU preference behavior. The additive broker-session and replay-hardening changes merged in Overhauled PR #93 justify advancing the independent CMake/Conan/vcpkg project version to 1.9.0:

- Enforce a hard cap of 1,024 broker client sessions and expose the operator-facing `--max-clients` bound.
- Add numeric IPv4 `--bind-address` and `--require-mtls-proxy`; strict mode refuses a public plaintext listener and requires loopback binding.
- Add an mTLS 1.3 stunnel deployment profile for client and server sides, preserving existing BRK1/BRK2 protocol framing.
- Add an optional single-process append-only replay journal; fsync accepted nonces before success and restore them on restart, with a 64 MiB file ceiling and fail-closed error handling.
- Keep default CPU-only builds and existing protocol contracts unchanged.

The security boundary remains limited: the stunnel peer certificate is not yet bound to the signed handoff sender, prepared artifacts and activation metadata remain in memory, and session I/O is still thread-per-client within its explicit ceiling. The journal is not a distributed replay database and does not make artifact staging restart-safe.

## Compatibility and security boundaries

- The handoff application contract is separate from WSM1 task/lifecycle messages; existing BRK1, BRK2, NXR1, MED1 and WSM1 framing identities are preserved.
- The receiver API is not wired into the ordinary BRK1/BRK2 TCP listener. The listener must not be exposed directly to untrusted networks.
- A deployment integration must provide mTLS 1.3 or an equivalent authenticated protected channel, bind the authenticated transport peer to the signed sender, and enforce frame limits before decoding.
- Replay and staged artifact state are process-local and in-memory. Restart-safe or multi-process deployments require durable shared replay and staging/activation services.
- The current Wasmtime C API composition runtime shares a Store/resource domain. It is not a multi-tenant sandbox; restrict it to operator-authorized artifacts or an independent process-isolation boundary.
- Application rate and memory budgets do not stop volumetric network DDoS. Apply firewall, reverse-proxy or provider edge controls where the service is exposed.
- Untrusted stream access does not require a WASM publisher key. Keep stream trust independent from executable-artifact trust.

## Release gate for 1.9.0

Before tagging or publishing:

1. Run the complete Overhauled CI suite on the exact candidate: default CPU-only build/tests, OpenSSL-enabled handoff tests, Wasmtime compile/runtime composition, paired YuKKi interoperability, BRK2 conformance, ASan/UBSan, package-consumer smoke tests and Conan package validation.
2. Verify the 1,024-session ceiling, `--max-clients` argument parsing, numeric bind validation and refusal of public listener bindings in strict mTLS-proxy mode.
3. Verify the replay journal across restart, duplicate nonce rejection, malformed/torn-journal fail-closed behavior, 64 MiB capacity and restrictive POSIX file permissions.
4. Validate stunnel configuration with test certificates and confirm TLS 1.3, server hostname validation, client-certificate verification and loopback forwarding.
5. Confirm paired YuKKi-OS BRK1/BRK2 interoperability and preserve all existing untrusted stream paths and independent stream flood limits.
6. Do not claim native TLS, transport-certificate-to-signed-sender binding, fixed event-driven I/O, durable artifact staging or restart-safe activation; those remain follow-up capabilities.
7. After required CI passes and the release commit is approved, create the 1.9.0 tag/GitHub Release. Only then compute exact source archive SHA-256/SHA-512 and update Homebrew/vcpkg registry recipes. Verify external package publication independently.

## Published release history

- v1.8.0 — published 2026-11-24; explicit BRK2 GPU preference for bounded workloads while preserving forced-CPU/exact-CPU constraints.
- v1.7.0 — published 2026-11-22; authenticated WASM receiver state machine, bounded admission and stream-flood controls.
- v1.6.0 — published 2026-11-22; opt-in authenticated-WASM receiver primitives and Wasmtime v51 C API correction. The receiver's full state machine, signed decisions/receipts, runtime bridge and bounded rate controls were added after that release by PR #86.
- v1.5.0 — published 2026-11-21; optional native Wasmtime runtime composition.
- v1.3.0 — published 2026-11-21; hierarchical worker topology, per-class CPU-group queues, bounded elasticity and large-host scheduling.
- v1.2.0 — published 2026-11-20; YuKKi-aware worker classes, broker-local NUMA affinity, process-wide admission budgets and BRK2 worker telemetry.
- v1.1.4 — published 2026-11-18; architecture/heuristic and release metadata alignment.
- v1.1.3 — published 2026-11-18; release metadata and prior broker hardening.
- v1.1.2 — prior published release.
