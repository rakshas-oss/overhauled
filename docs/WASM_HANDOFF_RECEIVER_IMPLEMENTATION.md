# WASM handoff receiver primitives

This opt-in implementation is the cryptographic and artifact-staging foundation
for the v1 handoff receiver. Configure with:

```sh
cmake -S . -B build -DENABLE_WASM_HANDOFF=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

OpenSSL Crypto (1.1.1 or newer) is required only when
`ENABLE_WASM_HANDOFF=ON`. The default broker build and the optional
`ENABLE_WASM_RUNTIME` Wasmtime build remain independent.

## Implemented in this change

- Ed25519 signature verification through OpenSSL EVP, using operator-provisioned
  raw public keys and exact issuer/subject/audience authorization tuples.
- SHA-256 parsing and hashing for artifact and chunk integrity.
- Bounded in-memory replay guard keyed by sender, audience and nonce.
- Strict sequential chunk staging with declared-size enforcement, per-chunk
  digest verification, final aggregate digest verification, and no access to
  staged bytes until the complete artifact verifies.

The signature verifier accepts the RFC 8785 canonical message bytes. This
library does not parse JSON or canonicalize it; the protocol decoder must
canonicalize the envelope before calling it. Trust keys must be provisioned
out-of-band and must never be accepted from the incoming message.

## Deliberate integration boundary

This is not yet a network-facing BRK2 message handler. It does not establish
mTLS or authenticated BRK2 peer identity, parse/validate the complete handoff
JSON schema, verify publisher provenance, persist replay state across
processes, issue signed prepare decisions/receipts, select a runtime backend,
or activate a release. The current TCP listener does not become authenticated
merely because these primitives are compiled in.

Do not expose the handoff endpoint or advertise a WASM handoff capability until
those integration stages are implemented and tested. In particular, a
process-local replay guard is appropriate only for a single receiver process;
multi-process deployments require shared replay storage or strict connection
routing to one replay authority.
