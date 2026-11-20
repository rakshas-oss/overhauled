# Overhauled versioning

## Current release candidate

- **Proposed version:** `1.2.0`
- **CMake project version:** `1.2.0`
- **Conan recipe version:** `1.2.0`
- **vcpkg manifest version:** `1.2.0`
- **Runtime/package identity:** `OverhauledTensorRuntime` / `nvlink_placement`
- **Release state:** candidate; tag, release archive, and external registry submissions are pending

## Version assessment

The 1.2.0 minor increment follows the published v1.1.4 baseline and the merged
benchmark/interop qualification work (#49–#51), plus the broker worker-fabric
feature in #53.

PR #53 adds five explicit worker classes (Control, RealtimeSpatial, Throughput,
ExactCpu, Inference), isolated worker queues and reserved pools, deadline-aware
EDF ordering for real-time spatial work, process-wide request/memory admission
budgets, Linux NUMA discovery and worker-thread CPU affinity, and queue/service
telemetry. It also extends BRK2 with optional request fields 11–13 and optional
response fields 8–12. Existing BRK1, BRK2 and WSM1 envelope identities and
required fields remain compatible; older peers can ignore the optional
non-critical request fields and response telemetry.

This is a new broker runtime capability and protocol extension, not merely a
documentation or benchmark patch. A minor increment to 1.2.0 is appropriate.
The prior 1.1.5 candidate is superseded by this feature release candidate; it
must not be described as published unless a matching tag/release is verified.

## Release policy

- The last verified published baseline is v1.1.4.
- Do not create or publish v1.2.0 until the release PR is reviewed and required
  C++ builds/tests, BRK2 conformance, sanitizers, package-consumer tests, and
  cross-repository live interoperability checks pass.
- Updating repository metadata does not publish Conan Center, vcpkg, Homebrew,
  apt or dnf packages. Verify each external registry separately.
- Refresh vcpkg source references/checksums and Homebrew archive URL/SHA-256
  only from the exact final tagged release archive. Do not invent checksums.
- YuKKi-OS owns workload meaning, spatial resolution, and policy directives.
  Overhauled validates/adopts worker class intent and owns local queue
  admission, Linux CPU/NUMA placement, backend selection and execution.
- Deadlines suppress expired queued/late results but do not preempt arbitrary
  native code or provide a hard real-time guarantee.

## Published release history

- `v1.1.4` — published 2026-11-18; architecture/heuristic and release metadata alignment.
- `v1.1.3` — published 2026-11-18; release metadata and prior broker hardening.
- `v1.1.2` — prior published release.
