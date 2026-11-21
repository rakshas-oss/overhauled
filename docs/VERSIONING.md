# Overhauled versioning

## Current release candidate

- **Proposed version:** `1.4.0`
- **CMake project version:** `1.4.0`
- **Conan recipe version:** `1.4.0`
- **vcpkg manifest version:** `1.4.0`
- **Runtime/package identity:** `OverhauledTensorRuntime` / `nvlink_placement`
- **Release state:** minor candidate; v1.4.0 tag and release archive are pending

## Version assessment

The 1.4.0 minor increment follows published v1.3.0 and adds the BRK2
locality-placement intent/result codec across the YuKKi-OS/Overhauled boundary:

- PR #57 isolates per-class queue contention, stabilizes worker CPU selection
  and avoids redundant affinity syscalls.
- PR #58 shards each worker class across eligible NUMA-local queues and adds
  optional bounded elastic worker capacity for RealtimeSpatial, Throughput
  and Inference, while Control and ExactCpu retain dedicated capacity.
- PRs #59–#62 add repeatable A/B throughput, pipelined mixed-class saturation,
  benchmark measurement-quality controls, and fixed-total worker-budget sweeps.
- PR #63 adds a validated machine-cpuset -> NUMA -> LLC/CPU-group hierarchy,
  per-group leaf queues, home-group/sibling-group/remote-NUMA work stealing,
  dynamically sized CPU masks, and bounded large-host sizing up to 16,384
  total workers (4,096 elastic).

These are additive runtime capabilities and a new scaling topology, not just
a performance-test or documentation update. A minor increment to 1.4.0 is
appropriate. The existing BRK1/BRK2/WSM1 wire contracts are preserved; the
new CPU-group placement detail is internal to broker scheduling.

## Release policy

- v1.3.0 is the latest published Overhauled release (2026-11-21).
- Do not create or publish v1.4.0 until its release PR is reviewed and required
  C++ builds/tests, worker-topology tests, BRK2 conformance, sanitizers,
  package-consumer tests and cross-repository interoperability checks pass.
- Updating this repository does not publish Conan Center, vcpkg, Homebrew,
  apt or dnf packages. Verify each external registry separately.
- The root vcpkg manifest is set to candidate 1.4.0; update the vcpkg portfile
  source reference and SHA512 only from the exact final tagged archive.
- The checked-in Homebrew formula still points to v1.1.1. Refresh its URL,
  version and SHA-256 only from the final v1.4.0 release archive.
- YuKKi-OS owns workload semantics, spatial resolution and directives.
  Overhauled owns local worker queues, admission budgets, CPU/NUMA/CPU-group
  placement, GPU topology, accelerator placement and execution.
- Deadlines order and reject expired queued work; they do not preempt arbitrary
  native code or provide a hard real-time guarantee.

## Published release history

- `v1.3.0` — published
- `v1.2.0` — published 2026-11-20; YuKKi-aware worker classes, broker-local
  NUMA affinity, process-wide admission budgets and BRK2 worker telemetry.
- `v1.1.4` — published 2026-11-18; architecture/heuristic and release
  metadata alignment.
- `v1.1.3` — published 2026-11-18; release metadata and prior broker hardening.
- `v1.1.2` — prior published release.

