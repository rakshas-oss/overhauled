# Interoperable Service Core — Overhauled implementation roadmap

**Status:** implementation roadmap; no transport-security or protocol-negotiation behavior is claimed by this document.

Overhauled owns the C++ execution plane: BRK1/BRK2 listener, session admission, broker dispatch, worker scheduling, runtime activation, cancellation execution, and applied placement reporting. YuKKi-OS owns workload policy, authenticated peer discovery, and the Rust-side broker client.

## Compatibility and trust invariants

- Preserve BRK1, BRK2, WSM1, NXR1, MED1, and YKMC identities and existing required fields.
- HELLO is optional and additive. It confirms capabilities and the listener's already-selected security profile; it must not negotiate TLS over plaintext.
- WAN listeners must reject plaintext. LAN and loopback policies are explicit deployment settings, not inferred from source IP or discovery labels.
- The ordinary broker data plane must not become a prerequisite for untrusted stream ingestion to possess an executable-artifact publisher key.
- Requested backend, NUMA node, or locality intent is not evidence of applied placement. Responses report only observed execution and successful affinity.
- Never accept peer-supplied precompiled Wasmtime artifacts for deserialization; compile verified source modules locally.

## Verified starting point

- The broker supports BRK1/BRK2 framing and bounded per-connection in-flight requests.
- The server currently tracks a thread per client session; it is not yet an event-driven or bounded worker-pool session I/O implementation.
- The authenticated WASM handoff receiver and optional Wasmtime runtime bridge exist as APIs, but the receiver is not wired into the ordinary BRK1/BRK2 listener.
- The native handoff path requires an authenticated protected transport and durable replay/staging integration to make restart-safe guarantees.
- Locality request/response fields exist, but physical memory placement is not yet applied and verified by the broker.
- Runtime interruption, cancellation acknowledgement, and durable handoff replay must be treated as separate acceptance gates, not assumed from API presence.

## Work packages

### OH0 — Bounded broker session I/O

- Replace thread-per-client session serving with a bounded event-driven or fixed worker-pool model; do not use the compute worker pools as socket-I/O workers.
- Preserve per-client in-flight limits, process-wide request and memory admission budgets, exact frame reads, socket deadlines, and graceful shutdown.
- Make max clients, session-I/O workers, pending requests, and queue limits configurable with safe upper bounds.
- Keep BRK1 and BRK2 listeners interoperable and preserve multi-request connection behavior.
- Tests: slowloris/partial-frame, concurrent sessions, disconnect during execution, saturation, shutdown, and bounded thread/memory counts.

### OH1 — Shared contract, HELLO, and CANCEL

- Consume a single normative contract repository or pinned contract package for schemas and executable fixtures.
- Audit existing message-type assignments and reserved values before assigning HELLO/CANCEL IDs. Do not assume proposed values are free.
- Implement Rust and C++ codecs from the same fixtures.
- HELLO confirms schema versions, frame limits, feature bits, observed transport profile, and authenticated peer key ID.
- CANCEL is cooperative: remove queued work where safe; interrupt running WASM only when fuel/epoch interruption is enabled; report GPU work as too late when it cannot be preempted.
- Emit a new cancellation status only to peers that negotiated it.
- Tests: no-HELLO compatibility, old-client enum safety, cancellation races, unknown fields, and shared corpus.

### OH2 — Authenticated control plane

- Add optional mTLS 1.3 listener profiles for loopback, LAN, and WAN.
- WAN profile must refuse plaintext; profile is fixed by listener configuration and confirmed, not selected, by HELLO.
- Bind the TLS peer identity to the handoff deployment-sender identity.
- Replace shared mesh PSK authorization with per-peer identities and key rotation in coordination with YuKKi-OS.
- Expose the observed transport profile and peer key ID to authorized diagnostics without leaking private key material.

### OH3 — Handoff execution and runtime safety

- Wire the authenticated receiver to the protected listener without exposing an unauthenticated artifact endpoint.
- Route eligible WSM1 tasks through the runtime bridge only after the task-to-release binding and execution contract are specified; preserve existing WSM1 semantics.
- Move module compilation off the receive path into a bounded compile pool with limits on concurrent compiles, input bytes, and wall time.
- Cache only locally compiled artifacts keyed by verified module digest.
- Persist replay nonces and staged/active release metadata before acknowledging durable state.
- Add Wasmtime fuel or epoch interruption and connect it to negotiated CANCEL.
- State the current operator-trusted-code boundary until per-tenant isolation is implemented.

### OH4 — Scale, locality, and operations

- Add zone-scoped discovery metadata and bounded seed/relay behavior in coordination with YuKKi-OS.
- Implement locality apply-and-report only where the allocator can prove placement; otherwise retain explicit experimental/unsupported outcomes.
- Add health endpoints that distinguish ready, draining, degraded, and failed states.
- Export session, queue wait, service time, backend, worker, applied NUMA, compile, handoff, and cancellation metrics.
- Keep stream-ingress flood controls independently configurable and bounded; bulk array data must use an explicitly provisioned bulk-data path rather than the untrusted-stream defaults.
- Remove or complete placeholder drivers only after a real hardware qualification path exists.

## Release gates

1. **A:** OH0 and OH1; shared corpus and live Rust/C++ interoperability green.
2. **B:** OH2; WAN plaintext refusal, identity pinning, and rotation tests green.
3. **C:** OH3; authenticated listener handoff, bounded compile, restart replay, WSM1 invocation, drain/release, and runtime interruption tests green.
4. **D:** OH4; multi-zone scale, health/metrics, placement truthfulness, and stream-flood tests green.

No version bump or release tag is implied by this roadmap. Release numbers are assigned only after the relevant implementation and CI gates pass.
