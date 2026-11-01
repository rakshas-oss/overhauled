# BRK2 Conformance v1

This document defines the first shared Rust/C++ conformance target for the BRK2 broker envelope. The byte corpus at `tests/fixtures/brk2/conformance-v1.json` is normative for the vectors it contains.

## Scope

Both implementations must agree on frame acceptance, header values, TLV field values, and schema-level treatment of critical versus optional extensions. This milestone does not change the wire format or claim complete conformance coverage.

## Wire invariants

- A frame is a 4-byte unsigned big-endian body length followed by exactly that many body bytes.
- The body begins with the 32-byte BRK2 header: magic, major/minor version, header length, message type, flags, request ID, schema ID, schema version, reserved field, and payload length.
- The header length is 32; the reserved field is zero; payload length equals the exact TLV region length.
- Each TLV is a 2-byte field ID, 1-byte wire type, 1-byte field flags, 4-byte big-endian value length, and that many value bytes.
- All multi-byte integers and lengths are big-endian. UUID values are 16 bytes in wire order.
- Unknown wire types and unknown flag bits are rejected. Unknown optional fields may be ignored or preserved; unknown critical fields are rejected during schema validation.
- Duplicate singular fields, truncated values, trailing frame bytes, malformed UTF-8, invalid fixed-width lengths, and configured-limit violations are rejected.
- A decoder must not panic, access out of bounds, or allocate based solely on an unchecked peer-provided length.

## Corpus semantics

The corpus distinguishes:
- `valid`: frame decoding and schema validation should succeed.
- `derived-invalid`: the specified mutation must be rejected.
- `derived-valid`: the specified mutation remains forward-compatible at the envelope layer; schema validation may ignore or preserve unknown optional fields.

The `expect` value `reject-at-schema-validation` means envelope decoding may succeed, but validation against the request schema must reject the unknown critical field. `accept-and-preserve-or-ignore` permits either safe preservation or omission of unknown optional fields; it does not permit interpreting them as known fields.

## Required parity checks

For every shared vector, the Rust and C++ runners must compare:
1. accept/reject outcome at the same validation layer;
2. decoded header and known field values;
3. request correlation ID preservation;
4. deterministic re-encoding for canonical known-field messages;
5. safe rejection of malformed frames without crash, hang, unbounded allocation, or cross-request state contamination.

Error strings are implementation details. Tests should compare stable error categories or the accept/reject layer, not exact human-readable text.

## Resource and stream behavior

Implementations must enforce explicit frame, payload, field-count, and per-field bounds before allocation or copying. Stream readers must tolerate partial reads, enforce a finite frame deadline, and release per-connection state on timeout or disconnect. Concurrency tests must verify request-ID correlation when multiple requests are in flight; no response may be attributed to a different request.

## Current milestone boundary

This commit adds the shared seed corpus and normative test expectations. It does not yet add corpus loaders to the Rust and C++ test binaries, sustained-load benchmarks, a coverage-guided fuzzer, or a complete timeout/concurrency harness. Those are follow-on implementation tasks; CI must not report this seed corpus as executed until both runners consume it.
