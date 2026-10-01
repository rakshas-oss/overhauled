# Broker Protocol (v1)

This repository exposes a broker-facing interoperability boundary for YuKKi-OS control-plane dispatch.

The current software release is v0.4 (`0.4.0`). The BRK1 broker wire protocol remains v1; this release does not change its frame format.

## Architecture Boundary

- YuKKi-OS: secure control plane and task orchestration
- overhauled: GPU topology, placement, and execution plane
- broker contract: versioned, bounded request/response protocol

No direct source dependency is introduced from this repository onto YuKKi-OS.

## Framing

Every message uses:

1. `uint32` big-endian frame length
2. protocol frame body

Frame body starts with:

- `magic` (`BRK1`)
- `version` (`1`)
- `message_type` (`request`/`response`)

All variable fields are length-prefixed and bounded by configured limits.

## Request Fields

- `task_id` (string)
- `source` (string)
- `destination` (string)
- `kind` (string)
- `priority` (`uint8`)
- `timeout_ms` (`uint32`)
- `payload` (binary)

## Response Fields

- `task_id` (string)
- `status` (`ok`/`rejected`/`error`/`timeout`)
- `selected_gpu` (`int32`, `-1` in CPU fallback mode)
- `latency_ms` (`uint64`)
- `result` (binary)
- `error` (string)

## Validation and Safety

- exact read/write loops for stream I/O
- strict frame length checks
- bounded payload/string/frame limits
- malformed input produces structured error responses
- bounded concurrent clients (`max_clients`)
- socket send/receive timeouts
- graceful shutdown of listener and active client sockets

## Async caller compatibility

- Broker transport framing remains `uint32_be frame_length` + BRK1/v1 binary frame body.
- YuKKi-OS Tokio/Wasmtime async callers can pipeline multiple requests on one TCP connection and match responses by `task_id`.
- Server enforces a bounded per-connection in-flight cap (`max_inflight_per_client`, default `32`) and returns a structured rejection when exceeded.
- If an upstream client still sends JSON `BrokerTask` / `BrokerResult` bodies, it is incompatible with this wire contract and must be adapted before traffic reaches `broker_server`.

## Execution Routing

- If GPUs are available, broker uses `nvlink::GpuTopology` + `nvlink::Placer`
- Requests are routed with sticky source affinity and queue-aware placement
- Compute path uses existing ADI compute entrypoint (`default_gpu_compute`)
- CPU-only deterministic mode is available (`--cpu-only`) for testing and non-GPU nodes

## Geospatial Frame Interoperability (`geospatial.frame.v1`)

When `BrokerRequest::kind == "geospatial.frame.v1"`, `request.payload` must be
an NXR1-encoded geospatial frame (see `include/geospatial_frame.h` /
`src/geospatial_frame.cpp`). This is a dedicated message kind handled inside
`BrokerService::handle_request`, independent of the default packed-`double`
compute path used for other kinds (e.g. `inference`).

### NXR1 wire format

All multi-byte fields are big-endian. The frame body is:

1. `magic` (`uint32`, `0x4E585231`, ASCII `NXR1`)
2. `version` (`uint16`, currently `1`)
3. `latitude` (`double`)
4. `longitude` (`double`)
5. `altitude` (`double`)
6. `velocity_x` (`double`)
7. `velocity_y` (`double`)
8. `velocity_z` (`double`)
9. `fluidity` (`double`)
10. `drag` (`double`)
11. `divergence` (`double`)
12. `payload_length` (`uint32`)
13. `payload` (opaque bytes, `payload_length` bytes)

This NXR1 frame is carried as the `payload` blob of a normal BRK1 request
(`kind = "geospatial.frame.v1"`); it is not a replacement for the BRK1
request/response transport.

### Validation and limits

- Exact magic (`0x4E585231`) and version (`1`) are required; mismatches are
  rejected with a descriptive error instead of being parsed opportunistically.
- All nine numeric fields must be finite (`NaN`/`Inf` are rejected on both
  encode and decode).
- `payload_length` is bounds-checked against the frame's remaining bytes
  (rejecting truncated frames) and against a configurable
  `max_payload_bytes` limit (defaults to the broker's
  `ProtocolLimits::max_payload_bytes`, 512 KiB).
- Any bytes remaining after the declared payload are rejected as trailing
  data; a conformant encoder never produces them.

### Broker handling

- A well-formed NXR1 payload is accepted (`TaskStatus::Ok`) and echoed back
  as the response `result`, letting a caller confirm a verified end-to-end
  round trip through the broker without side effects.
- A malformed NXR1 payload (bad magic/version, truncation, trailing bytes,
  oversized payload, or non-finite numbers) is rejected with
  `TaskStatus::Rejected` and a structured `error` message; it never falls
  through to the double-array compute path used by other `kind` values.

### Build/test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_CUDA=OFF -DENABLE_TENSORRT=OFF -DBUILD_BROKER=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -R 'geospatial_frame_test|broker_service_test'
```

### Coordination with YuKKi-OS

This repository only owns the NXR1 payload contract and the `overhauled`-side
broker handling of `geospatial.frame.v1`. Any YuKKi-OS-side NXR1 encoder/
decoder or client integration must be validated against this exact wire
format (and against the real YuKKi-OS broker client code) before being
treated as interoperable; this document is the source of truth for the byte
layout on the `overhauled` side.

## GPU-Backed WASM Sandbox Interoperability (`wasm.task.v1`, `wasm.lifecycle.*`)

When `BrokerRequest::kind` is `wasm.task.v1` or starts with `wasm.lifecycle.`, `request.payload` carries a `WSM1`-encoded binary frame for WebAssembly sandbox execution and safe hotswap management.

### Supported kinds
- `wasm.task.v1`: Submits a GPU acceleration task from a WASM sandbox.
- `wasm.lifecycle.prepare.v1`: Registers/prepares a new module version for hotswap.
- `wasm.lifecycle.drain.v1`: Initiates graceful draining of an old module version (rejects new tasks with `ModuleDraining`).
- `wasm.lifecycle.release.v1`: Releases GPU placement records once in-flight tasks have drained to 0.
- `wasm.lifecycle.query.v1`: Queries version status and active task counts.
- `wasm.lifecycle.v1`: Generic lifecycle message (action specified in WSM1 payload).

For the full wire specification, byte layout, Rust client contract, and safe hotswap sequence, see [docs/WASM_INTEROP.md](WASM_INTEROP.md).

