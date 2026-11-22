# Overhauled versioning

## Release state

- **Latest published version:** 1.6.0 (2026-11-22).
- **Next minor release candidate:** 1.7.0 (not tagged or published).
- **CMake project version:** 1.7.0 in this candidate.
- **Conan recipe version:** 1.7.0 in this candidate.
- **Root vcpkg manifest:** 1.7.0 in this candidate.
- **Published vcpkg registry baseline and Homebrew formula:** remain pinned to immutable 1.6.0 sources until the 1.7.0 tag archive and its checksums are available.
- **Package identity:** OverhauledTensorRuntime / nvlink_placement.

No 1.7.0 tag, GitHub Release, Homebrew update, vcpkg registry entry, or external package upload is claimed by this candidate.

## Version assessment

The additive receiver and admission capabilities merged in PR #86 justify a minor increment from 1.6.0:

- A prepare/transfer/activation receiver state machine verifies signed deployment messages and artifact publisher provenance in separate trust domains.
- Ed25519 identities are bound to exact issuer, subject, audience and key ID values; expiry, nonce/replay and signed message validation are enforced.
- Strict sequential chunk transfer verifies per-chunk, module and aggregate digests before activation.
- Signed prepare decisions and activation receipts report the receiver's selected backend and engine/ABI information.
- Global and per-sender message and ingress-byte token buckets operate alongside bounded sender tracking, session counts, module/chunk sizes and total staged-byte ceilings.
- The optional Wasmtime runtime bridge validates and compiles a complete staged module set, then publishes the release through the configured activation callback. Failed validation or activation leaves the previous active release unchanged.
- The untrusted NXR1, MED1 and media/stream-relay paths remain separately accessible without executable-artifact publisher authorization; their own framing, size, connection, rate and backpressure limits continue to apply.

## Compatibility and security boundaries

- The handoff application contract is separate from WSM1 task/lifecycle messages; existing BRK1, BRK2, NXR1, MED1 and WSM1 framing identities are preserved.
- The receiver API is not wired into the ordinary BRK1/BRK2 TCP listener. The listener must not be exposed directly to untrusted networks.
- A deployment integration must provide mTLS 1.3 or an equivalent authenticated protected channel, bind the authenticated transport peer to the signed sender, and enforce frame limits before decoding.
- Replay and staged artifact state are process-local and in-memory. Restart-safe or multi-process deployments require durable shared replay and staging/activation services.
- The current Wasmtime C API composition runtime shares a Store/resource domain. It is not a multi-tenant sandbox; restrict it to operator-authorized artifacts or an independent process-isolation boundary.
- Application rate and memory budgets do not stop volumetric network DDoS. Apply firewall, reverse-proxy or provider edge controls where the service is exposed.
- Untrusted stream access does not require a WASM publisher key. Keep stream trust independent from executable-artifact trust.

## Release gate for 1.7.0

Before tagging or publishing:

1. Run the complete Overhauled CI suite on the exact release candidate, including the OpenSSL-enabled receiver conformance tests and the Wasmtime bridge composition test.
2. Confirm paired YuKKi-OS sender/Overhauled receiver interoperability and the independent untrusted-stream regression tests.
3. Confirm default CPU-only builds remain unchanged and optional CUDA, Ethos-U85 and Wasmtime paths remain feature-gated.
4. After the release tag exists, calculate the exact source-archive SHA-256/SHA-512 values and update the Homebrew and vcpkg recipes/registry database. Do not insert guessed or placeholder checksums.
5. Verify each external package publication independently; a manifest or recipe in this repository is not evidence of an upload.

PR #86 (receiver state machine, runtime bridge and rate limits) is merged to main at 019e762ddbf0ba2c69777732ecc1c9e9c6540a2e. This version candidate records the feature set; its own CI and release approval remain separate.

## Published release history

- v1.6.0 — published 2026-11-22; opt-in authenticated-WASM receiver primitives and Wasmtime v51 C API correction. The receiver's full state machine, signed decisions/receipts, runtime bridge and bounded rate controls were added after that release by PR #86.
- v1.5.0 — published 2026-11-21; optional native Wasmtime runtime composition.
- v1.3.0 — published 2026-11-21; hierarchical worker topology, per-class CPU-group queues, bounded elasticity and large-host scheduling.
- v1.2.0 — published 2026-11-20; YuKKi-aware worker classes, broker-local NUMA affinity, process-wide admission budgets and BRK2 worker telemetry.
- v1.1.4 — published 2026-11-18; architecture/heuristic and release metadata alignment.
- v1.1.3 — published 2026-11-18; release metadata and prior broker hardening.
- v1.1.2 — prior published release.
