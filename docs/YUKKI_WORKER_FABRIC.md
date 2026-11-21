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

Each worker class has an ordered queue on every eligible CPU group (normally
an LLC-sharing group) nested inside a NUMA node. Each leaf queue has its own
mutex; class-specific wake channels avoid waking unrelated worker classes.
FIFO is preserved within a leaf queue. RealtimeSpatial uses earliest-deadline
first within each leaf queue when a deadline is present; equal deadlines
preserve admission order and deadline-bearing work precedes work without a
deadline. Deadlines are not globally ordered across different leaf queues.

Workers search for work hierarchically: their home CPU-group queue first,
sibling groups in the same NUMA parent second, and remote NUMA parents last.
A stolen task executes on the worker's home CPU, so its execution telemetry
reports the worker's actual pinned NUMA node. Work stealing is class-preserving:
a dedicated worker steals only its own class, and elastic workers still obey
the process-wide per-class service ceilings.

Leaf routing follows this precedence:

1. A preferred NUMA node narrows routing to eligible CPU groups beneath that
   parent when such capacity exists.
2. A logical spatial-affinity key selects a stable eligible CPU group.
3. Without either hint, requests are distributed round-robin across eligible
   CPU groups for that worker class.

A caller-provided NUMA hint is advisory, never a CPU ID or permission to widen
the broker's affinity. A class receives a leaf queue only when a dedicated or
permitted elastic worker can drain it; this prevents orphaned queues. If a
requested NUMA node has no eligible leaf for that class, routing safely falls
back to another eligible domain.

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
a hard maximum of 4,096. The combined dedicated-plus-elastic process limit is
16,384. The default is zero, preserving the dedicated-only worker budget until
elastic capacity is explicitly enabled. Automatic sizing uses up to one worker
per discovered physical core, capped at 4,096; larger deployments can opt in
to a larger explicit budget after measuring stack memory, queue pressure and
service behavior.

Elastic service is restricted to three classes:

| Class | Default concurrent elastic ceiling | Policy |
|---|---:|---|
| RealtimeSpatial | 1 | Deadline queue ordering is retained |
| Throughput | 4 | Primary source of elastic batch capacity |
| Inference | 2 | Shares only its configured elastic ceiling |
| Control | 0 | Dedicated capacity only |
| ExactCpu | 0 | Dedicated CPU-only capacity |

Each elastic worker searches from its home CPU group through the same
hierarchy used by dedicated workers. It selects eligible classes with weighted
rotation (3 RealtimeSpatial : 4 Throughput : 2 Inference slots), subject to a
process-wide concurrent-service ceiling per class. A busy class cannot consume
another class's configured elastic allowance. Elastic workers never service
Control or ExactCpu.

Dedicated worker IDs occupy five disjoint 16,384-ID class ranges. The elastic
worker ID range 81,920–86,015 identifies elastic workers. The existing BRK2
response schema is unchanged; CPU-group identity is available only in the
broker's in-process WorkerExecutionContext, not as a new wire field.
The ceilings can be tuned with --elastic-realtime-max N,
--elastic-throughput-max N, and --elastic-inference-max N.

## NUMA placement

On Linux, topology discovery builds a bounded three-level tree:

- Machine root: the CPU set allowed by the broker process's dynamically sized
  `sched_getaffinity` mask.
- NUMA children: node CPU lists intersected with that process-allowed set.
- CPU-group leaves: the highest shared-cache CPU groups discovered from
  `/sys/devices/system/cpu/cpu*/cache/index*/shared_cpu_list`, intersected
  with their NUMA parent.

Every child CPU set must be a subset of its parent. Sibling NUMA domains and
sibling CPU groups must be disjoint, and children must exactly cover their
parent. Discovery rejects malformed or oversized CPU lists, and bounds the
CPU count, NUMA-node count and CPU-group fanout. If cache metadata is
incomplete, the scheduler safely falls back to one CPU group per NUMA node;
if the NUMA tree itself is incomplete, it falls back to one process-local
domain. It never expands the process-allowed CPU set.

The worker fabric uses dynamically allocated Linux CPU masks, so CPU IDs above
the legacy fixed `CPU_SETSIZE` limit remain representable. Worker ordinals are
distributed across leaf CPU groups in proportion to their allowed CPU count.
A worker pins itself to its assigned CPU; the response reports the NUMA node
only when affinity application succeeds. The CPU-group ID is retained in
in-process execution context for diagnostics.

The discovered topology is cached as the process-startup placement boundary.
Apply cgroup/cpuset changes before launching the broker; restart the process
after changing its allowed CPU set or NUMA placement.

On non-Linux systems, restricted containers, or hosts where affinity cannot
be applied, worker execution continues without claiming NUMA pinning. A
logical spatial key is not a physical CPU ID, geographic coordinate, or
promise of hard real-time execution.

### Nested-domain security boundary

The topology tree is a placement boundary, not a tenant-authentication system.
Untrusted request metadata cannot supply arbitrary CPU IDs, expand the root
cpuset, create child domains, or bypass a parent NUMA/CPU-group boundary.
Preferred NUMA hints are validated against eligible leaf queues and are
substituted when they are unavailable.

The worker and topology limits bound thread creation and topology parsing.
The process-wide admission count and in-flight byte budget still protect
request memory; nested CPU groups do not create independent tenant quotas.
For mutually untrusted tenants, run separate broker processes/containers with
disjoint cgroup cpusets and memory limits. Do not treat scheduler nesting as
a security boundary between tenants sharing one broker identity.

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
Inference. Remaining workers go to Throughput. Automatic sizing discovers the
process-allowed topology, targets one worker per physical core, and caps the
default at 4,096 workers. Explicit configuration can request up to 16,384
combined dedicated and elastic threads, subject to the process-wide limit.

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

The combined dedicated-plus-elastic worker count must be 5..16,384.
Dedicated Control, RealtimeSpatial, ExactCpu, and Inference pools each need
at least one worker; Throughput also retains at least one worker. Elastic
threads are added after the reserved workers; if `--workers-throughput` is
omitted, `--workers TOTAL` or the automatic budget determines the Throughput
remainder after accounting for elastic slots. Automatic sizing is capped at
4,096; explicit sizing supports larger CPU hosts up to the 16,384-thread cap.

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
