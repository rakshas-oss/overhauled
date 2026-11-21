# BRK2 Locality Placement Extension v1

Status: proposal for cross-repository implementation. This document specifies an optional BRK2 request/response extension; it does not claim that a host allocator currently honours placement requests.

## Ownership and trust boundary

YuKKi-OS owns workload placement policy and decides whether locality is preferred or mandatory. Overhauled owns broker-local execution and the allocation of broker-owned request-payload memory. Overhauled may report placement as honoured only after the allocator responsible for that exact allocation confirms success.

A placement request is intent, never authority. It cannot widen the broker process cpuset, bypass memory quotas, expose raw topology, or override local admission policy. The broker validates all remote node hints against its own allowed memory policy.

## Compatibility and versioning

The BRK2 envelope remains major version 2, schema IDs remain request=1 and response=2, and existing fields 1–13 are unchanged. This extension adds optional request field 14 and optional response field 13. The nested extension has its own version field, initially 1.

- Prefer MUST use a non-critical outer field. Older peers may ignore it; the client must treat a missing placement result as unknown/not applied, never as success.
- Bind and interleave MUST use a critical outer field. An older peer that does not understand field 14 rejects it using the existing unknown-critical-field rule; it must not silently ignore a mandatory constraint.
- A peer that understands field 14 but cannot satisfy a mandatory request returns broker status Rejected with a placement result explaining why.
- Unknown optional nested fields may be ignored. Unknown critical nested fields and duplicate singular nested fields reject the extension.

This does not add JSON HELLO negotiation, transport authentication, or a second discovery system. A capability advertisement is advisory only; actual processing is authoritative.

## Request field 14: placement intent

Outer TLV: field ID 14, wire type Nested (11), critical flag selected as above.

The nested value is a sequence of standard 8-byte BRK2 TLV headers and values:

| Nested field | Wire type | Required | Meaning |
|---|---|---:|---|
| 1 | U16 | yes | Extension version; exactly 1 |
| 2 | U8 | yes | Scope; 1 = broker_request_payload |
| 3 | U8 | yes | Mode; 1 = prefer, 2 = bind, 3 = interleave |
| 4 | U32 | conditional | Preferred/bound NUMA node; required for prefer and bind, absent for interleave |
| 5 | U64 | yes | Maximum bytes subject to this request; nonzero and no greater than decoded payload size |

V1 intentionally scopes placement to the broker-owned request-payload copy. It does not claim placement of YuKKi-OS WASM linear memory, caller-owned datasets, accelerator staging buffers, GPU VRAM, or NPU memory. These allocations have different owners and need separately scoped contracts.

Prefer permits a safe local fallback. Bind requires the specified node or rejection. Interleave requests distribution across the allocator's eligible local NUMA nodes; it does not expose a node list or CPU mask.

The node identifier is opaque within the sender's negotiated locality domain. It is not a CPU ID, cpuset, or globally meaningful machine identifier. If the receiver cannot establish a compatible locality domain, it rejects bind; for prefer, it may fall back and report that result.

## Response field 13: placement result

Outer TLV: field ID 13, wire type Nested (11), non-critical. It is present only when field 14 was supplied and processed.

Nested fields:

| Nested field | Wire type | Required | Meaning |
|---|---|---:|---|
| 1 | U16 | yes | Extension version; exactly 1 |
| 2 | U8 | yes | Scope; 1 = broker_request_payload |
| 3 | U8 | yes | Result; 1 = honored, 2 = rejected, 3 = fallback |
| 4 | U32 | conditional | Applied NUMA node; present only when a single node was actually applied |
| 5 | U16 | conditional | Reason code; required for rejected/fallback |

Reason codes: 1 unsupported_scope, 2 unsupported_mode, 3 locality_domain_mismatch, 4 node_unavailable, 5 process_policy_denied, 6 memory_quota, 7 allocator_unavailable, 8 size_limit, 9 placement_failed.

Never report honored merely because a worker thread was pinned to a NUMA node. The result must reflect allocator-level success for the exact requested allocation. For interleave, omit field 4. For fallback, include a reason. For rejection, broker task status is Rejected.

If field 14 is absent, field 13 is absent. If a legacy peer ignores an optional prefer field, field 13 is absent and the client records the result as unknown, not honoured.

## Bounds and validation

- Bound the nested value and every inner TLV by existing BRK2 frame/payload limits.
- Accept exactly one outer field 14 and at most one each of nested fields 1–5.
- Reject zero or over-payload max_bytes, invalid enums, invalid conditional node presence, unsupported extension versions, malformed nested TLVs, and duplicate singular fields.
- Apply a strict local cap before allocator calls; never allocate based only on peer-provided max_bytes.
- Validate node IDs against the receiver's allowed memory policy and process constraints.
- Never serialize raw CPU masks, machine topology, physical addresses, or allocator handles.

## Required conformance cases

Rust and C++ runners must share vectors for: optional prefer accepted by an old peer as unknown optional; critical bind rejected by an old peer; v1 prefer/bind/interleave decoded identically; unknown optional nested field accepted; unknown critical nested field rejected; duplicate nested singular field rejected; invalid scope/mode/node combinations rejected; zero/oversized max_bytes rejected; honored/fallback/rejected response decoding; absent result treated as unknown; and mandatory placement failure correlated to the original request ID.

## Implementation boundary

This specification is not allocator implementation or NUMA qualification. The runtime must wire the codec and explicit result reporting before enabling a platform allocator. Allocation backends must be separately tested for the broker payload copy and report allocator-level success/failure. Other memory classes remain unsupported until their owning process implements and verifies them.
