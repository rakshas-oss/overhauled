# Authenticated WASM handoff contract v1 — Overhauled receiver

Normative envelope: `protocol/wasm-handoff-v1.schema.json`. Both repositories carry the same schema.

## Receiver responsibilities

The opt-in `HandoffReceiver` API implements the receiving-side prepare, transfer,
verification and activation state machine:

- Verify the signed deployment envelope against out-of-band Ed25519 trust,
  exact issuer/subject/audience and bounded lifetime.
- Verify publisher provenance with a separate trust store. A trusted transport
  sender is not automatically a trusted artifact publisher.
- Enforce unique message IDs/nonces through the replay guard; bound concurrent
  preparations and total staged bytes.
- Accept only bounded, strictly sequential chunks. Check each chunk digest and
  the complete per-module digest before exposing verified bytes to activation.
- Validate required capabilities and local limits. The current policy selects
  only a configured Wasmtime backend and fails closed when its validator or
  activation callback is not configured.
- Issue signed prepare decisions and activation receipts identifying the
  backend, engine/version and ABI profile.
- Preserve the previous active release unless module validation and the
  configured runtime's atomic activation callback both succeed.

The receiver applies global and per-sender message rate limits, global and
per-sender ingress-byte token buckets, a bounded sender-rate table, maximum
concurrent sessions, maximum total staged bytes, maximum module size and
maximum chunk size. Defaults are 100 messages/s globally (burst 200), 20
messages/s per sender (burst 40), 32 MiB/s global ingress (16 MiB burst),
8 MiB/s per sender (8 MiB burst), 16 concurrent preparations, 512 MiB
staged-byte ceiling, 64 MiB per module, 256 MiB per artifact set and 4 MiB
per chunk. Policy configuration is itself capped: global message rate
at 10,000/s, per-sender message rate at 1,000/s, global ingress rate at
1 GiB/s, per-sender ingress at 256 MiB/s, global byte burst at 64 MiB,
per-sender byte burst at 16 MiB, global message burst at 10,000, per-sender
message burst at 1,000, and tracked sender identities at 1,024. Operators may
reduce these values but cannot configure larger values.

## Trust, transport and replay

Envelope authentication is Ed25519 over RFC 8785 canonical JSON with the
signature member omitted. Trust keys are provisioned out of band. The receiver
checks audience, issuer, subject, key ID, expiry, nonce uniqueness and
publisher provenance independently of service discovery.

The library API is not itself a network listener. A future or external
transport adapter must provide mutually authenticated TLS 1.3 (or an
equivalently authenticated protected channel), bind the authenticated peer
identity to the signed envelope sender, enforce frame limits before JSON
allocation, and return rate-limit failures without buffering rejected data.
The ordinary BRK1/BRK2 TCP listener remains unauthenticated; this change does
not make it safe to expose directly to an untrusted network.

Replay state and prepared artifact staging are currently process-local and
in-memory. Multi-process or restart-safe deployments must provide shared,
durable replay storage and a durable staging/activation implementation before
claiming that guarantee.

## Runtime selection and isolation

`runtime_policy=auto` is not permission to weaken requirements. The current
receiver accepts only a configured Wasmtime engine whose version, ABI and
required capabilities satisfy local policy. It rejects absent validators or
activation callbacks. The configured `module_validator` must perform real
Wasmtime compilation/validation. The `activate_release` callback must stage
the complete release and atomically switch active runtime state; it must own
or copy module bytes before returning.

The present Overhauled C API composition runtime shares one Store/resource
domain. It is not a multi-tenant sandbox. Until fuel/epoch interruption,
per-task isolation/teardown and fine-grained WASI grants are implemented and
qualified, restrict this handoff path to operator-authorized trusted artifacts
or an independently isolated process boundary. Do not advertise a backend
merely because a callback can be installed.

## Stream trust is separate

The handoff trust policy applies only to executable deployment artifacts.
It does not gate NXR1 geospatial frames, MED1 media streams, BRK stream payloads
or YuKKi stream-relay ingestion. Those paths continue to accept untrusted
data under their own framing, size, memory, subscriber, connection, rate and
backpressure limits. A stream publisher does not need to possess a WASM
publisher key merely to send or view stream data.

## Lifecycle and conformance

`DISCOVERED -> AUTHORIZED -> PREPARE_ACCEPTED -> TRANSFERRING -> VERIFIED ->
VALIDATED -> ACTIVATED`. Preparation and activation are separate; activation
is local, not a distributed atomic transaction.

The C++ receiver test covers Ed25519 sender/publisher trust, signed decisions,
chunk integrity, activation receipts, per-sender throttling and failed-
validation rollback. YuKKi's sender tests cover canonical message construction,
publisher provenance, per-chunk signatures, bounded streaming and identity
pinning. The TLS/network adapter, durable replay service and a real deployment
through the broker listener remain integration requirements, not completed
capabilities.
