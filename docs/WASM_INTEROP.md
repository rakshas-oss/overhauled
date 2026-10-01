# GPU-Backed WASM Sandbox Interoperability Specification

This document defines the wire contract, lifecycle operations, and host/broker interface for executing GPU-backed WebAssembly (WASM) sandbox tasks from **YuKKi-OS** (`rakshas-oss/YuKKi-OS`) on **overhauled** (`rakshas-oss/overhauled`).

The current overhauled software release is v0.4 (`0.4.0`). This release does not change the interoperability wire contracts: BRK1 and WSM1 remain protocol v1.

---

## 1. System Boundary and Architectural Scope

```text
┌────────────────────────────────────────────────────────┐
│                   YuKKi-OS Host Plane                  │
│                                                        │
│  ┌────────────────────────┐  ┌──────────────────────┐  │
│  │ WASM Sandbox (v1.0.0)  │  │ WASM Sandbox (v2.0)  │  │
│  │ (Wasmtime sandbox mem) │  │  (hotswap target)    │  │
│  └───────────┬────────────┘  └──────────┬───────────┘  │
│              │ host call / IPC          │              │
│  ┌───────────▼──────────────────────────▼───────────┐  │
│  │            YuKKi-OS WASM GPU Adapter             │  │
│  │     (Rust client, Tokio async TCP stream)        │  │
│  └───────────────────────┬──────────────────────────┘  │
└──────────────────────────┼─────────────────────────────┘
                           │ TCP (BRK1 framing + WSM1 payload)
                           │ or in-process C++ Host API
┌──────────────────────────▼─────────────────────────────┐
│                    overhauled Broker                   │
│                                                        │
│  - NVLink GPU topology detection & affinity placement  │
│  - Fractional lane scheduling & queue management       │
│  - WSM1 wire validation & idempotency correlation      │
│  - Safe hotswap lifecycle tracking (Prepare/Drain/Rel) │
│  - Execution on CUDA GPU or deterministic CPU fallback │
└────────────────────────────────────────────────────────┘
```

### What overhauled provides
- GPU topology awareness (NVLink / PCIe) and optimal device placement.
- Sticky affinity per sandbox / tenant ID to minimize memory reallocations.
- Queue-depth tracking and backpressure.
- Module version registration and safe hotswap state transitions.
- Task execution on CUDA-capable GPUs or deterministic CPU fallback.

### Explicit Limitations and Non-Goals
1. **WASM Bytecode Execution**: overhauled is **not** a WebAssembly runtime. It does not parse, validate, or execute WASM bytecode or JIT compilation. WASM execution remains solely the responsibility of the YuKKi-OS host (e.g. Wasmtime).
2. **Sandbox Isolation**: overhauled does not enforce WASM linear memory bounds, memory sandboxing, or capability tokens. The YuKKi-OS host adapter must validate sandbox memory before handing data descriptors to overhauled.
3. **No Unsafe Live GPU State Migration**: During module hotswaps, overhauled does **not** attempt to migrate raw CUDA pointers, texture memory, or live device context across module versions. The sandbox/host must initialize state in the new module version. overhauled ensures old tasks drain safely and releases device placement resources when instructed.

---

## 2. WSM1 Wire Format Specification

All multi-byte numeric fields are encoded in **big-endian (network byte order)**. Strings are length-prefixed with a 2-byte (`uint16_be`) length. Binary payloads are length-prefixed with a 4-byte (`uint32_be`) length.

### Constants and Enums

```text
WASM_MAGIC            = 0x57534D31  ("WSM1" ASCII)
WASM_PROTOCOL_VERSION = 1

MessageType:
  1 = TaskRequest
  2 = TaskResponse
  3 = LifecycleRequest
  4 = LifecycleResponse

TaskStatus:
  0 = Ok
  1 = Rejected
  2 = Error
  3 = Timeout
  4 = ModuleDraining
  5 = ModuleNotFound
  6 = VersionMismatch

LifecycleAction:
  1 = Prepare   (register & prepare new module version)
  2 = Drain     (stop accepting new tasks on old module version)
  3 = Release   (release GPU/broker resources after drain)
  4 = Query     (query status of registered version)

LifecycleStatus:
  0 = Ok
  1 = Rejected
  2 = Error
  3 = Busy
  4 = NotFound

ModuleState:
  0 = Unknown
  1 = Prepared
  2 = Active
  3 = Draining
  4 = Stopped
  5 = Released

BufferAccessFlags:
  1 = Read
  2 = Write
  3 = ReadWrite
```

### Buffer Descriptor Layout

Each buffer descriptor is serialized as:
- `buffer_id` (`uint32_be`)
- `flags` (`uint32_be`)
- `offset` (`uint64_be`)
- `length` (`uint64_be`)
- `name_len` (`uint16_be`)
- `name` (`name_len` UTF-8 bytes)

---

### Frame 1: WSM1 TaskRequest (`msg_type = 1`)

| Offset / Field | Type | Description |
|---|---|---|
| `0..3` | `uint32_be` | Magic: `0x57534D31` (`WSM1`) |
| `4..5` | `uint16_be` | Protocol version: `1` |
| `6` | `uint8` | Message type: `1` (`TaskRequest`) |
| `7` | `uint8` | Reserved flags: `0` |
| `8..9` | `uint16_be` | `task_id_len` |
| Variable | UTF-8 bytes | `task_id` (idempotency ID) |
| Next 2 | `uint16_be` | `sandbox_id_len` |
| Variable | UTF-8 bytes | `sandbox_id` (sandbox identity) |
| Next 2 | `uint16_be` | `module_id_len` |
| Variable | UTF-8 bytes | `module_id` (module identity) |
| Next 2 | `uint16_be` | `module_version_len` |
| Variable | UTF-8 bytes | `module_version` (e.g. `"1.0.0"`) |
| Next 2 | `uint16_be` | `task_kind_len` |
| Variable | UTF-8 bytes | `task_kind` (e.g. `"inference"`) |
| Next 1 | `uint8` | `priority` (0-255) |
| Next 8 | `uint64_be` | `deadline_ms` (absolute deadline or relative timeout) |
| Next 2 | `uint16_be` | `buffer_count` |
| Variable | Descriptors | Array of `WasmBufferDescriptor` |
| Next 4 | `uint32_be` | `payload_len` |
| Variable | Bytes | `payload` data |

---

### Frame 2: WSM1 TaskResponse (`msg_type = 2`)

| Offset / Field | Type | Description |
|---|---|---|
| `0..3` | `uint32_be` | Magic: `0x57534D31` (`WSM1`) |
| `4..5` | `uint16_be` | Protocol version: `1` |
| `6` | `uint8` | Message type: `2` (`TaskResponse`) |
| `7` | `uint8` | `status` (enum `TaskStatus`) |
| `8..9` | `uint16_be` | `protocol_version` (`1`) |
| `10..11` | `uint16_be` | `task_id_len` |
| Variable | UTF-8 bytes | `task_id` |
| Next 4 | `int32_be` | `selected_gpu` (`-1` for CPU fallback) |
| Next 8 | `uint64_be` | `latency_ms` (execution latency) |
| Next 2 | `uint16_be` | `error_len` |
| Variable | UTF-8 bytes | `error` (empty string if Ok) |
| Next 2 | `uint16_be` | `buffer_count` |
| Variable | Descriptors | Array of output `WasmBufferDescriptor` |
| Next 4 | `uint32_be` | `result_len` |
| Variable | Bytes | Result payload data |

---

### Frame 3: WSM1 LifecycleRequest (`msg_type = 3`)

| Offset / Field | Type | Description |
|---|---|---|
| `0..3` | `uint32_be` | Magic: `0x57534D31` (`WSM1`) |
| `4..5` | `uint16_be` | Protocol version: `1` |
| `6` | `uint8` | Message type: `3` (`LifecycleRequest`) |
| `7` | `uint8` | `action` (1=Prepare, 2=Drain, 3=Release, 4=Query) |
| `8..9` | `uint16_be` | `request_id_len` |
| Variable | UTF-8 bytes | `request_id` (correlation/idempotency ID) |
| Next 2 | `uint16_be` | `sandbox_id_len` |
| Variable | UTF-8 bytes | `sandbox_id` |
| Next 2 | `uint16_be` | `module_id_len` |
| Variable | UTF-8 bytes | `module_id` |
| Next 2 | `uint16_be` | `module_version_len` |
| Variable | UTF-8 bytes | `module_version` |
| Next 4 | `int32_be` | `target_gpu` (`-1` = auto placement) |
| Next 4 | `uint32_be` | `grace_period_ms` (for drain) |
| Next 2 | `uint16_be` | `ack_token_len` |
| Variable | UTF-8 bytes | `ack_token` (lease token for release) |
| Next 4 | `uint32_be` | `payload_len` |
| Variable | Bytes | Optional configuration/metadata |

---

### Frame 4: WSM1 LifecycleResponse (`msg_type = 4`)

| Offset / Field | Type | Description |
|---|---|---|
| `0..3` | `uint32_be` | Magic: `0x57534D31` (`WSM1`) |
| `4..5` | `uint16_be` | Protocol version: `1` |
| `6` | `uint8` | Message type: `4` (`LifecycleResponse`) |
| `7` | `uint8` | `action` (echoed from request) |
| `8` | `uint8` | `status` (enum `LifecycleStatus`) |
| `9..10` | `uint16_be` | `protocol_version` (`1`) |
| `11` | `uint8` | `state` (enum `ModuleState`) |
| `12..13` | `uint16_be` | `request_id_len` |
| Variable | UTF-8 bytes | `request_id` |
| Next 2 | `uint16_be` | `sandbox_id_len` |
| Variable | UTF-8 bytes | `sandbox_id` |
| Next 2 | `uint16_be` | `module_id_len` |
| Variable | UTF-8 bytes | `module_id` |
| Next 2 | `uint16_be` | `module_version_len` |
| Variable | UTF-8 bytes | `module_version` |
| Next 4 | `int32_be` | `assigned_gpu` |
| Next 4 | `uint32_be` | `active_tasks` (current in-flight count) |
| Next 2 | `uint16_be` | `lease_token_len` |
| Variable | UTF-8 bytes | `lease_token` |
| Next 2 | `uint16_be` | `error_len` |
| Variable | UTF-8 bytes | `error` |

---

## 3. BRK1 TCP Broker Transport Integration

When communicating over TCP with `broker_server`, the WSM1 binary frames are wrapped in standard `BRK1` broker frames:

```text
[4-byte uint32_be frame_length]
  [BRK1 header (magic=0x42524b31, version=1, type=Request, reserved=0)]
  [task_id string]
  [source string]
  [destination string]
  [kind string: "wasm.task.v1" or "wasm.lifecycle.v1"]
  [priority uint8]
  [timeout_ms uint32_be]
  [payload: WSM1 TaskRequest or WSM1 LifecycleRequest binary frame]
```

### Supported `BrokerRequest::kind` values:
- `wasm.task.v1`: Submits a WASM task. Payload must be a WSM1 `TaskRequest`.
- `wasm.lifecycle.v1`: Generic lifecycle operation (action in WSM1 payload).
- `wasm.lifecycle.prepare.v1`: Module version prepare/register.
- `wasm.lifecycle.drain.v1`: Module version drain.
- `wasm.lifecycle.release.v1`: Module version resource release.
- `wasm.lifecycle.query.v1`: Module version status query.

---

## 4. Safe Hotswap Sequence

```text
YuKKi-OS Control Plane                              overhauled Broker
         │                                                  │
         │ 1. Lifecycle Prepare (v2.0.0)                    │
         ├─────────────────────────────────────────────────►│ Assigns GPU placement,
         │◄─────────────────────────────────────────────────┤ sets state = Prepared,
         │    Ok, assigned_gpu, lease_token                 │ returns lease token
         │                                                  │
         │ 2. (Host restores/initializes sandbox state)     │
         │                                                  │
         │ 3. Lifecycle Drain (v1.0.0)                      │
         ├─────────────────────────────────────────────────►│ Sets v1.0 state = Draining.
         │◄─────────────────────────────────────────────────┤ Rejects any NEW v1.0 tasks
         │    Ok, state=Draining, active_tasks=N            │ with ModuleDraining.
         │                                                  │
         │ 4. Switch traffic to v2.0.0                      │
         │    TaskRequest (v2.0.0)                          │
         ├─────────────────────────────────────────────────►│ State becomes Active.
         │◄─────────────────────────────────────────────────┤ Executes on assigned GPU.
         │                                                  │
         │ 5. In-flight tasks on v1.0 complete              │
         │    (active_tasks reaches 0 => Stopped)           │
         │                                                  │
         │ 6. Lifecycle Release (v1.0.0, ack_token)         │
         ├─────────────────────────────────────────────────►│ Checks active_tasks == 0.
         │◄─────────────────────────────────────────────────┤ Releases GPU placement record.
         │    Ok, state = Released                          │
```

---

## 5. Rust Client Implementation Reference (No C++ Headers Required)

Below is an example Rust implementation that can be directly pasted into YuKKi-OS:

```rust
use std::io::{Cursor, Read, Write};
use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};

pub const WASM_MAGIC: u32 = 0x57534D31;
pub const WASM_PROTOCOL_VERSION: u16 = 1;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WasmBufferDescriptor {
    pub buffer_id: u32,
    pub flags: u32,
    pub offset: u64,
    pub length: u64,
    pub name: String,
}

#[derive(Debug, Clone)]
pub struct WasmTaskRequest {
    pub task_id: String,
    pub sandbox_id: String,
    pub module_id: String,
    pub module_version: String,
    pub task_kind: String,
    pub priority: u8,
    pub deadline_ms: u64,
    pub buffer_descriptors: Vec<WasmBufferDescriptor>,
    pub payload: Vec<u8>,
}

pub fn encode_task_request(req: &WasmTaskRequest) -> Vec<u8> {
    let mut w = Vec::new();
    w.write_u32::<BigEndian>(WASM_MAGIC).unwrap();
    w.write_u16::<BigEndian>(WASM_PROTOCOL_VERSION).unwrap();
    w.write_u8(1).unwrap(); // msg_type = TaskRequest
    w.write_u8(0).unwrap(); // flags = 0

    let write_str = |buf: &mut Vec<u8>, s: &str| {
        buf.write_u16::<BigEndian>(s.len() as u16).unwrap();
        buf.extend_from_slice(s.as_bytes());
    };

    write_str(&mut w, &req.task_id);
    write_str(&mut w, &req.sandbox_id);
    write_str(&mut w, &req.module_id);
    write_str(&mut w, &req.module_version);
    write_str(&mut w, &req.task_kind);

    w.write_u8(req.priority).unwrap();
    w.write_u64::<BigEndian>(req.deadline_ms).unwrap();

    w.write_u16::<BigEndian>(req.buffer_descriptors.len() as u16).unwrap();
    for buf in &req.buffer_descriptors {
        w.write_u32::<BigEndian>(buf.buffer_id).unwrap();
        w.write_u32::<BigEndian>(buf.flags).unwrap();
        w.write_u64::<BigEndian>(buf.offset).unwrap();
        w.write_u64::<BigEndian>(buf.length).unwrap();
        write_str(&mut w, &buf.name);
    }

    w.write_u32::<BigEndian>(req.payload.len() as u32).unwrap();
    w.extend_from_slice(&req.payload);

    w
}

#[derive(Debug, Clone)]
pub struct WasmTaskResponse {
    pub status: u8,
    pub protocol_version: u16,
    pub task_id: String,
    pub selected_gpu: i32,
    pub latency_ms: u64,
    pub error: String,
    pub buffer_descriptors: Vec<WasmBufferDescriptor>,
    pub result: Vec<u8>,
}

pub fn decode_task_response(bytes: &[u8]) -> Result<WasmTaskResponse, String> {
    let mut r = Cursor::new(bytes);
    let magic = r.read_u32::<BigEndian>().map_err(|e| e.to_string())?;
    if magic != WASM_MAGIC {
        return Err("invalid magic".into());
    }
    let ver = r.read_u16::<BigEndian>().map_err(|e| e.to_string())?;
    if ver != WASM_PROTOCOL_VERSION {
        return Err("unsupported version".into());
    }
    let msg_type = r.read_u8().map_err(|e| e.to_string())?;
    if msg_type != 2 {
        return Err("unexpected msg_type".into());
    }
    let status = r.read_u8().map_err(|e| e.to_string())?;
    let protocol_version = r.read_u16::<BigEndian>().map_err(|e| e.to_string())?;

    let read_str = |cursor: &mut Cursor<&[u8]>| -> Result<String, String> {
        let len = cursor.read_u16::<BigEndian>().map_err(|e| e.to_string())? as usize;
        let mut buf = vec![0u8; len];
        cursor.read_exact(&mut buf).map_err(|e| e.to_string())?;
        String::from_utf8(buf).map_err(|e| e.to_string())
    };

    let task_id = read_str(&mut r)?;
    let selected_gpu = r.read_i32::<BigEndian>().map_err(|e| e.to_string())?;
    let latency_ms = r.read_u64::<BigEndian>().map_err(|e| e.to_string())?;
    let error = read_str(&mut r)?;

    let buf_count = r.read_u16::<BigEndian>().map_err(|e| e.to_string())? as usize;
    let mut buffer_descriptors = Vec::with_capacity(buf_count);
    for _ in 0..buf_count {
        let buffer_id = r.read_u32::<BigEndian>().map_err(|e| e.to_string())?;
        let flags = r.read_u32::<BigEndian>().map_err(|e| e.to_string())?;
        let offset = r.read_u64::<BigEndian>().map_err(|e| e.to_string())?;
        let length = r.read_u64::<BigEndian>().map_err(|e| e.to_string())?;
        let name = read_str(&mut r)?;
        buffer_descriptors.push(WasmBufferDescriptor { buffer_id, flags, offset, length, name });
    }

    let res_len = r.read_u32::<BigEndian>().map_err(|e| e.to_string())? as usize;
    let mut result = vec![0u8; res_len];
    r.read_exact(&mut result).map_err(|e| e.to_string())?;

    Ok(WasmTaskResponse {
        status,
        protocol_version,
        task_id,
        selected_gpu,
        latency_ms,
        error,
        buffer_descriptors,
        result,
    })
}
```
