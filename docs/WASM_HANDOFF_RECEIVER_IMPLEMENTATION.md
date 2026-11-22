# WASM handoff receiver

This opt-in library implements the receiver-side application state machine and
cryptographic/artifact verification primitives. It does **not** yet add a
network-facing BRK2 handler to the ordinary broker listener.

Configure and run the receiver unit test with:

```sh
cmake -S . -B build -DENABLE_WASM_HANDOFF=ON -DENABLE_CUDA=OFF
cmake --build build --target wasm_handoff_receiver_test
ctest --test-dir build --output-on-failure -R wasm_handoff_receiver_test
```

OpenSSL Crypto (1.1.1 or newer) is required only when
`ENABLE_WASM_HANDOFF=ON`. The default broker build and optional
`ENABLE_WASM_RUNTIME` Wasmtime build remain independent.

## Implemented

- Ed25519 sign/verify helpers using OpenSSL EVP, with operator-provisioned
  raw keys and exact issuer/subject/audience trust tuples.
- Canonical JSON construction for prepare, artifact chunk, activation,
  signed prepare decision and signed receipt messages.
- Separate publisher-attestation verification; transport sender trust does
  not confer permission to publish artifacts.
- Bounded message replay guard, global/per-sender message token buckets, and
  global/per-sender ingress-byte token buckets with bounded subject tracking.
- Bounded concurrent prepare sessions, total staged-byte accounting, bounded
  module/chunk lengths, strict increasing chunk offsets and per-chunk/final
  SHA-256 verification.
- Prepare/transfer/activation state handling, configured compile-only module
  validation callback, atomic runtime activation callback, and signed receipt.
- Failed validation/activation discards the new prepared session and leaves
  the receiver's prior active-release record untouched.

## Required deployment configuration

The receiver fails closed unless local policy supplies:

- An authorized sender trust store and a separate publisher trust store.
- A receipt signing key and a receipt audience.
- A Wasmtime module validator that actually compiles/validates each complete
  staged module.
- An activation callback that owns/copies the verified bytes and atomically
  promotes the complete release. A failed callback must not change the
  previously active runtime.

The receiver currently supports the Wasmtime engine family only. It does not
infer availability from discovery labels or from a remote preferred-backend
hint. The callback is responsible for mapping the validated release into the
configured runtime.

## Deliberate integration boundaries

- **No network listener yet:** no BRK2 or HTTP message handler is wired to
  `HandoffReceiver`. The caller must authenticate a TLS 1.3 peer (or an
  equivalent protected channel), bind that peer identity to the signed
  envelope sender, apply frame bounds before allocating/parsing the JSON
  envelope, and map signed decisions/receipts to the shared schema.
- **Process-local replay and staging:** `ReplayGuard`, prepared bytes and the
  active receipt map live in one process. Multi-process or restart-safe
  deployments require shared durable replay storage and durable staging.
- **No tenant sandbox claim:** the currently shared-Store Wasmtime embedding
  is not an untrusted multi-tenant isolation boundary. Run trusted artifacts
  only or isolate the receiver/runtime in a separate process until the
  necessary fuel, interruption, WASI grant and per-task teardown controls are
  implemented and qualified.
- **Streams are not gated by this trust store:** NXR1/MED1 stream ingestion
  remains available to untrusted publishers and is constrained by separate
  frame limits, bounded storage, rate limits, connection quotas and
  backpressure.

Do not advertise an authenticated network handoff endpoint or general
untrusted-code sandbox until the corresponding transport and runtime
integration requirements are implemented and tested.
