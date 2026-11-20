# YuKKi-OS-Aware Worker Fabric

This document describes the broker-side execution contract for YuKKi-OS-directed
work. YuKKi-OS owns workload meaning and emits an optional BRK2 directive.
Overhauled owns bounded admission, local class queues, Linux CPU/NUMA affinity,
accelerator dispatch, and execution telemetry.

The repositories remain separate. This feature does not move Wasmtime,
NXR1 interpretation, spatial freshness policy, or numerical semantics into
Overhauled.

![Overhauled broker worker-fabric architecture](diagrams/overhauled-architecture.svg)

The diagram summarizes the five isolated queues, their ordering rules,
process-wide admission controls, broker-local Linux NUMA affinity, backend
dispatch and BRK2 response telemetry. It is paired with the terminal renderer
at `docs/diagrams/print-ansi.sh`.

## Worker classes

BRK2 request TLVs extend the existing request schema without changing the
BRK2 envelope version or the BRK1/BRK2/WSM1 protocol identities.

| Class | Intended work | Local execution policy |
|---|---|---|
| `control` | Protocol and lifecycle coordination | Dedicated worker pool; CPU execution |
| `realtime-spatial` | Time-sensitive spatial streams | Dedicated pool; earliest-deadline-first when a deadline is supplied, otherwise FIFO; logical affinity and optional NUMA hint |
| `throughput` | Dataset, batch, and parallel workloads | Dedicated pool; normal local backend policy |
| `exact-cpu` | Exactness, verification, and crypto-sensitive workloads | Dedicated pool; CPU forced |
| `inference` | Model inference | Dedicated pool; local NPU/GPU policy applies |

An explicit class is workload intent, not an unconditional hardware command.
Overhauled validates the directive and retains final authority over local
capacity and backend availability. A device preference refines the class but
cannot override the CPU-only policy of `control` or `exact-cpu`.

BRK1 clients and BRK2 clients that omit field 11 retain the legacy
`throughput` scheduling default, but the legacy request is not treated as an
explicit Throughput backend profile. This preserves existing BRK1 backend
selection semantics.

## BRK2 request and response fields

Optional request fields:

| Field | Wire type | Meaning |
|---:|---|---|
| 11 | UTF-8 | `control`, `realtime-spatial`, `throughput`, `exact-cpu`, or `inference` |
| 12 | U64 | Stable logical spatial-affinity key |
| 13 | U32 | Preferred NUMA node ID |

A `realtime-spatial` directive must include field 12. Field 13 is advisory:
the broker only reports a NUMA node when it successfully applies CPU affinity
within the process's allowed CPU mask.

Existing response field 7 continues to report the selected backend. Additional
optional response fields are:

| Field | Wire type | Meaning |
|---:|---|---|
| 8 | UTF-8 | Worker class actually dispatched |
| 9 | U64 | Queue wait in microseconds |
| 10 | U64 | Service time in microseconds |
| 11 | U32 | Broker worker ID |
| 12 | U32 | NUMA node successfully applied, when available |

Responses rejected before dispatch omit worker telemetry. Older clients can
ignore optional response fields; older brokers can ignore optional non-critical
request fields. The WSM1 payload is unchanged.

## Queue ordering, capacity, and overload protection

Each worker class has a separate ordered queue on every NUMA node
where that class can be serviced. Each queue has its own mutex and condition
variable; enqueueing wakes a worker in that node/class rather than unrelated
pools. FIFO is preserved within each node-local queue. RealtimeSpatial uses
earliest-deadline-first when a deadline is present; equal deadlines preserve
admission order and deadline-bearing work precedes work without a deadline.
Different NUMA shards execute independently, and there is no cross-class
global ordering.

Node routing follows this precedence:

1. A preferred NUMA node is used when that class has a worker queue on it.
2. Otherwise a logical spatial-affinity key selects a stable eligible node.
3. Without either hint, requests are distributed round-robin across the
   nodes eligible for that worker class.

A class is eligible on a node only when a dedicated worker or a permitted
elastic worker can drain that node's queue. This prevents requests from being
placed in an orphaned queue. A requested node may be substituted when that
class has no worker capacity on it.

The queues have no fixed per-class entry limit. They allocate on demand, but
the broker still applies a process-wide admission budget to prevent memory
exhaustion:

- At most 65,536 outstanding requests process-wide.
- By default, the request count is derived from
  `max_clients * max_inflight_per_client`, capped at 65,536. Defaults are
  128 clients and 32 in-flight requests per client (4,096 outstanding).
- The default in-flight memory budget is 64 MiB. Accounting includes request
  payload bytes, bounded routing strings, and fixed request metadata.
- Each admitted request holds its count and memory reservation until execution
  completes, including after the client socket disconnects.
- Exhausted budgets and allocation failures reject admission safely. They do
  not terminate a worker thread or bypass the limit after reconnect.

Configure limits with `--max-outstanding N` (zero derives the default) and
`--max-inflight-bytes N`. Do not raise them without measuring resident memory
under worst-case payload sizes and concurrent clients.

The per-client in-flight cap remains active. Global admission accounting also
closes the disconnect/reconnect loophole: queued work retains its reservation
until its completion guard runs.

## Bounded class-aware elastic workers

Elastic workers are optional additional threads (--workers-elastic N), with
a hard maximum of 64 and an overall dedicated-plus-elastic process limit of
256. The default is zero, preserving the dedicated-only worker budget until
elastic capacity is explicitly enabled.

Elastic service is restricted to three classes:

| Class | Default concurrent elastic ceiling | Policy |
|---|---:|---|
| RealtimeSpatial | 1 | Deadline queue ordering is retained |
| Throughput | 4 | Primary source of elastic batch capacity |
| Inference | 2 | Shares only its configured elastic ceiling |
| Control | 0 | Dedicated capacity only |
| ExactCpu | 0 | Dedicated CPU-only capacity |

Each elastic worker services only queues on its own NUMA node. It selects
eligible classes with weighted rotation (3 RealtimeSpatial : 4 Throughput :
2 Inference slots), subject to a process-wide concurrent-service ceiling per
class. A busy class cannot consume another class's configured elastic
allowance. Elastic workers never service Control or ExactCpu.

The worker ID telemetry range 1280–1343 identifies elastic workers. The
existing BRK2 response schema is unchanged. The elastic flag is available in
the broker's in-process WorkerExecutionContext, not as a new wire field.
The ceilings can be tuned with --elastic-realtime-max N,
--elastic-throughput-max N, and --elastic-inference-max N.

## NUMA placement

On Linux, the worker fabric reads node CPU lists from
`/sys/devices/system/node/node*/cpulist`, intersects them with the process's
current `sched_getaffinity` mask, and applies worker-thread affinity with
`pthread_setaffinity_np`.

A request is routed to a node-local class queue before execution.
If its preferred node has an eligible queue for that class, the broker uses
it; otherwise the logical affinity key selects an eligible node, or requests
without either hint are distributed round-robin. Dedicated and elastic
workers consume only queues on their assigned home node.

On Linux, a worker pins itself to a CPU in that queue's node. CPU selection is
deterministic for the worker, and the last successfully applied node/CPU pair
is cached to avoid redundant affinity syscalls. The response reports a NUMA
node only when affinity application succeeds.

On non-Linux systems, restricted containers, or hosts where affinity cannot
be applied, worker execution continues without claiming NUMA pinning. A
logical spatial key is not a physical CPU ID, geographic coordinate, or
promise of hard real-time execution.

## Deadlines and timeouts

The broker computes a monotonic deadline at admission from the smaller of the
relative request timeout and the optional absolute BRK2 Unix-nanosecond
deadline. The RealtimeSpatial queue uses that converted monotonic deadline for
within-class earliest-deadline-first admission. If the task expires while queued, it returns Timeout without
starting execution. The remaining deadline budget is passed into the broker
service, and a result completed after the deadline is not returned as success.

This is a cooperative request deadline around broker execution. It cannot
preempt an already-running GPU kernel or guarantee that arbitrary native code
will stop at an exact wall-clock instant.

## Worker sizing

Defaults reserve one worker each for Control, RealtimeSpatial, ExactCpu, and
Inference. Remaining workers go to Throughput. With automatic sizing, the
total target is `min(256, max(5, 2 * hardware_concurrency))`.

Example:

```bash
./build/broker_server 9090 \
  --workers-control 1 \
  --workers-realtime 2 \
  --workers-throughput 8 \
  --workers-exact-cpu 1 \
  --workers-inference 2 \
  --workers-elastic 4 \
  --elastic-realtime-max 1 \
  --elastic-throughput-max 4 \
  --elastic-inference-max 2 \
  --max-outstanding 4096 \
  --max-inflight-bytes 67108864
```

The combined dedicated-plus-elastic worker count must be 1..256.
Dedicated Control, RealtimeSpatial, ExactCpu, and Inference pools each need
at least one worker. Elastic threads are added after the reserved workers; if
`--workers-throughput` is omitted, `--workers TOTAL` or the automatic
budget determines the Throughput remainder after accounting for elastic slots.
The legacy automatic worker target remains `min(256, max(5, 2 *
hardware_concurrency))`; enabling elastic workers can increase the total
above that target when needed to retain at least one Throughput worker.

## Operational measurements

BRK2 response telemetry supplies queue wait, service time, worker class,
worker ID, and applied NUMA node. Use those values with client-side p50/p95/p99
latencies, backend counts, rejected admissions, and resident memory.

Benchmark representative YuKKi-OS drone, finance, and environmental datasets
through the live BRK2 broker. Include mixed-class concurrency, class
saturation, client disconnect/reconnect churn, oversized payloads, deadlines
that expire while queued, and CPU fallback. A microbenchmark that bypasses
the broker worker pools does not validate this execution fabric.

## Compatibility and ownership

- BRK1, BRK2, and WSM1 envelope identities are unchanged.
- New BRK2 request metadata is optional and non-critical.
- BRK1 requests continue on the legacy Throughput queue and existing backend
  semantics.
- YuKKi-OS chooses the class and logical workload hints.
- Overhauled validates and applies local admission, NUMA placement, and
  accelerator policy.
- NXR1 payload bytes are opaque to this scheduling layer.
