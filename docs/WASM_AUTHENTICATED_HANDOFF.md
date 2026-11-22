# Authenticated WASM handoff contract v1 — Overhauled receiver

Normative envelope: `protocol/wasm-handoff-v1.schema.json`.

## Receiver responsibilities
- Advertise only backends actually built, enabled and tested on this node. Do not advertise native Wasmtime when `ENABLE_WASM_RUNTIME=OFF`.
- Authenticate the peer independently of mesh discovery. Bind BRK2/mTLS peer identity to signed sender and audience.
- Verify Ed25519 signatures over RFC 8785 canonical JSON, trust, authorization, expiry and nonce uniqueness before accepting bytes.
- Verify artifact publisher provenance separately from transport sender identity.
- Stage bounded BRK2 chunks privately; verify per-chunk, module, manifest and aggregate digests before compilation. Reject gaps, overlaps, conflicting duplicate chunks and excess sizes.
- Select a backend from live local capabilities, enforce the stricter resource ceilings, and issue signed decisions/receipts containing actual backend ID, engine version, ABI and accepted capabilities.
- Keep prepare, activation and execution separate. Failed preparation must not change active release.

## Backend selection
`runtime_policy=auto` permits a compatible local choice but never weakens required semantics, ABI, capability or isolation. Reject if no backend qualifies. Do not silently add host imports or change WASI profile. Do not route untrusted tenants to the current shared-Store Wasmtime embedding as though it were isolated.

The optional C API backend may be advertised only when built with `ENABLE_WASM_RUNTIME=ON`, and only for verified engine versions, targets and ABI profiles. WSM1 remains a task/lifecycle protocol and does not carry executable modules; this handoff is a separate versioned BRK2 application protocol.

## Security and conformance
The current Wasmtime C API embedding shares a Store/resource domain. Fuel metering, epoch interruption, fine-grained WASI grants and per-task teardown remain prerequisites for an untrusted-code isolation profile. Until then use trusted artifacts or a separately isolated process.

Run the shared contract vectors for authentication, replay, artifact integrity, chunk boundaries, capability escalation, unsupported imports/features, backend mismatch, explicit no-backend rejection, cancellation and prepare rollback. Receipts must identify the backend actually used.
