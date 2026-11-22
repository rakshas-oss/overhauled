# Security hardening notes

This note summarizes defensive checks applied to Overhauled's placement, broker framing, and optional TensorRT runtime.

- GPU placement rejects an empty topology before round-robin selection.
- The TensorRT model registry synchronizes model publication and lookup. Loading/compilation occurs outside the registry lock.
- BRK2 task IDs remain subject to the configured string limit; oversized error messages are truncated to fit. Payload size limits are unchanged.
- Enabling interop encryption fails clearly at configure time if its implementation source is absent.
- TensorRT runtime input is bounded, read and written in exact byte loops, and validated before dispatch. ONNX paths are constrained by the runtime's accepted path policy.

The TCP/TensorRT endpoint is a binary protocol endpoint, not an authentication layer. Expose it only on a trusted network or behind an authenticated transport boundary.

## Authenticated WASM handoff

- `HandoffReceiver` validates Ed25519 deployment-sender messages against operator-provisioned trust entries bound to exact issuer, subject and audience values. Artifact-publisher provenance is checked through a separate trust store; possession of a deployment key does not authorize arbitrary publisher content.
- The receiver enforces message lifetime, nonce/replay checks, sequential chunk offsets, per-chunk and complete-module SHA-256 digests, maximum module/artifact sizes, concurrent preparation and staged-byte ceilings.
- Global and per-sender message-rate and ingress-byte token buckets use bounded sender tracking. Rate-limit rejection occurs before staging additional artifact bytes.
- The optional Wasmtime bridge compiles the complete staged release in a new runtime/store and promotes the runtime only after successful validation. Failed validation/activation leaves the old active runtime pointer unchanged.
- The receiver is a library API, not a network endpoint. An adapter must supply authenticated protected transport, bind TLS peer identity to the signed sender, enforce frame limits before allocation and provide durable replay/staging when restart-safe guarantees are required.
- The current shared-Store Wasmtime embedding is not a multi-tenant sandbox. Restrict it to trusted operator-authorized artifacts or an independent process-isolation boundary.

## Untrusted stream availability

- NXR1/MED1 stream and media-relay ingestion is not gated by the WASM publisher trust store. Untrusted stream sources remain usable under independent framing, retained-memory, connection, rate and backpressure limits.
- In-process flood limits bound application work but cannot stop volumetric traffic from saturating an upstream network link. Use host firewall, authenticated gateway/reverse proxy and provider edge DDoS controls where exposed.