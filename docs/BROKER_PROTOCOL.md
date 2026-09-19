# Broker Protocol (v1)

This repository exposes a broker-facing interoperability boundary for YuKKi-OS control-plane dispatch.

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
