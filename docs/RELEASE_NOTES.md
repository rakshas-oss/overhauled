## v1.4.0 (release candidate; tag pending)
Date: 2026-11-21

### Highlights

- **BRK2 locality-placement codec:** add bounded nested-TLV decoding for request field 14 and response field 13, including extension-version, scope, mode, conditional NUMA-node, payload-size, duplicate-field, unknown-critical-field, and criticality/mode validation.
- **Compatibility semantics:** `prefer` remains non-critical for older peers; `bind` and `interleave` are critical and cannot be silently ignored.
- **Placement outcome semantics:** an absent response result means unknown/not applied, not success.

### Explicit implementation boundary

This release adds protocol codec support only. It does not add a broker-owned payload allocator, apply or verify physical memory placement, or claim NUMA-local allocation. The field-14/field-13 contract is limited to the broker-owned request-payload copy; WASM linear memory, datasets, accelerator staging, GPU VRAM and NPU memory remain out of scope.

### Compatibility and release gate

- Existing BRK1/BRK2/WSM1 envelope identities remain unchanged; the new BRK2 fields are additive and versioned.
- Require full broker CI, BRK2 C++ protocol tests, sanitizer checks, package-consumer smoke tests and live YuKKi-OS interoperability before release approval.
- Do not tag or publish v1.4.0 until the release PR is reviewed, all required checks pass, and the exact source archive checksums are generated.

---

## v1.3.0 (published 2026-11-21)
Date: 2026-11-20

### Highlights

- **Hierarchical worker topology:** added validated machine-cpuset -> NUMA ->
  LLC/CPU-group placement domains. Child sets remain inside their parents,
  sibling groups are disjoint, and the topology must cover the process-allowed
  CPU set without broadening it.
- **Dynamic CPU affinity masks:** Linux placement supports CPU IDs beyond the
  legacy `CPU_SETSIZE` boundary, with bounded cpulist parsing and topology
  limits.
- **Per-class CPU-group queues:** each worker class has ordered CPU-group
  leaf queues. Workers prefer their home group, steal within sibling groups
  next, and consider remote NUMA domains last. Stealing remains class-preserving.
- **Bounded large-host worker capacity:** explicit sizing supports up to
  16,384 total workers and 4,096 elastic workers. Automatic sizing targets
  physical cores and is capped at 4,096.
- **Admission and scheduler contention:** worker admission uses atomic
  bounded reservations; queue locks and wakeups are isolated by worker class
  and CPU group.
- **Reproducible measurement:** follow-up PRs add repeatable throughput A/B,
  pipelined mixed-class saturation, measurement-quality controls and
  fixed-total worker-budget sweeps.

### Compatibility and release boundary

- The work builds on published v1.2.0 worker classes, deadlines, budgets,
  NUMA placement and BRK2 worker telemetry. It does not change BRK1/BRK2/WSM1
  envelope identities or add CPU-group IDs to the wire protocol.
- CPU-group placement and worker stealing are internal broker scheduling
  details. Preferred NUMA and affinity hints cannot widen the broker's
  process-allowed CPU set.
- The v1.3.0 release was published after topology,
  scheduler, BRK2, sanitizer, package-consumer and cross-repository CI gates
  pass and the release commit is approved.
- The vcpkg archive SHA512 and Homebrew archive URL/SHA256 must be generated
  from the exact final v1.3.0 release archive. External registries are
  submitted and verified separately.

---

## v1.2.0 (published 2026-11-20)
Date: 2026-11-19

### Highlights

- **YuKKi-aware broker worker fabric:** adds Control, RealtimeSpatial,
  Throughput, ExactCpu and Inference queues with reserved worker pools.
- **Deadline-aware spatial scheduling:** RealtimeSpatial uses earliest-deadline
  first ordering for deadline-bearing requests, with stable admission-order
  tie breaking; expired queued work is not started and late results are not
  reported as success.
- **Admission and memory protection:** process-wide outstanding-request and
  in-flight memory budgets retain reservations through execution completion,
  including client disconnects.
- **Linux NUMA placement:** discovers node CPU sets, intersects them with the
  process-allowed CPU mask, and reports NUMA placement only after successful
  worker-thread affinity application.
- **BRK2 worker metadata:** optional request fields 11–13 carry class, logical
  spatial affinity and preferred NUMA node; optional response fields 8–12
  report class, queue wait, service time, worker ID and applied NUMA node.
- **Benchmark and operations guidance:** adds class/affinity/NUMA/deadline
  benchmark controls and operational guidance for mixed-class saturation,
  deadlines and resource budgets.

### Compatibility and release boundary

- BRK1, BRK2 and WSM1 envelope identities and existing required fields remain
  unchanged. New BRK2 request fields are optional/non-critical and response
  telemetry is optional.
- BRK1 and legacy BRK2 requests retain the Throughput queue and legacy backend
  selection semantics.
- NUMA affinity is implemented for Linux and may be unavailable in restricted
  environments. Deadlines are not native-code preemption or hard real-time
  guarantees.
- CMake, Conan and vcpkg repository metadata were aligned to 1.2.0. The
  v1.2.0 GitHub tag and release were published on 2026-11-20. External
  package-manager submissions must still be verified independently.

---

# Release Notes

Repository: rakshas-oss/overhauled

## v1.1.5 (release candidate; tag pending)
Date: 2026-11-18

### Highlights

- **Dataset benchmark input hardening**: validates manifest root type, dataset
  and profile object/ID shape, duplicate IDs, uint64 sequence/timestamp bounds,
  sampling controls, dataset file containment and documented size ceilings.
- **BRK2 response telemetry validation**: validates envelope/schema, request
  and task correlation, status, backend identity and selected-GPU consistency.
  Benchmark output reports actual backend counts, selected GPU IDs and CPU
  fallback counts; requested preferences are not treated as execution proof.
- **Cross-repository benchmark bridge**: documented YuKKi-OS browser spatial
  WASM measurements as local Node execution plus an optional direct BRK2
  inference leg using host-prepared numeric records. This is separate from
  WSM1 guest task/lifecycle execution and does not send Wasm modules to broker.
- **Architecture diagrams and ownership boundaries**: added full-colour SVG
  and terminal diagrams clarifying the broker, host runtime, NUMA and
  accelerator responsibility split.

### Compatibility and release boundary

- The work since v1.1.4 is benchmark/tooling, validation, tests and
  documentation. It does not intentionally change broker runtime dispatch,
  public C++ APIs, or BRK1/BRK2/WSM1 wire layouts.
- CMake, Conan and the root vcpkg manifest are aligned to 1.1.5. The vcpkg
  portfile checksum and Homebrew URL/SHA-256 must be refreshed from the exact
  final release archive before registry submission.
- v1.1.5 is a candidate only. Do not create its tag or describe package
  registries as updated until the release commit and each registry submission
  have been verified.

---

## v1.1.4 (published 2026-11-18)
Date: 2026-11-18

### Highlights

- **Architecture heuristic audit**: Documented the actual backend-selection
  order, the distinction between BRK2 preferences and proven execution, the
  CPU safety rules, fallback-safety requirements, and the default GPU
  offload/backlog thresholds.
- **Topology interpretation**: Clarified that the detector's 100/90/10
  peer-link values are relative placement hints derived from CUDA peer
  attributes, not measured bandwidth or portable latency guarantees.
- **NPU execution boundary**: Aligned the interface comments and operations
  guidance with the CPU-emulated conformance executor and optional Linux
  Ethos-U DRM implementation. A service enable flag alone does not create an
  executor; simulator results are not hardware performance evidence.
- **WASM host/broker boundary**: Preserved the documented rule that YuKKi-OS
  compiles, links, instantiates and composes Wasm programs, while Overhauled
  accepts host-mediated task data and module lifecycle metadata.
- **Step-by-step operations guide**: Added a measured heuristic-verification
  sequence for CPU, GPU and NPU configurations, including fallback-safe and
  fallback-unsafe checks.

### Compatibility and release boundary

- This was a documentation and release-metadata maintenance release. No
  BRK1/BRK2 or WSM1 wire format, public C++ API, or broker runtime behavior was
  intentionally changed.
- The v1.1.4 GitHub release was published on 2026-11-18. External package-
  manager registry submissions are separate and must be verified independently.
- The current v1.1.5 candidate builds on that published baseline with benchmark
  input and BRK2 response-validation hardening.

---

## v1.1.3
Date: 2026-11-18

### Highlights

- **Release metadata alignment**: Synchronized the CMake project version, Conan recipe, vcpkg manifest, README release badge, and deployment instructions at 1.1.3.
- **Documentation maintenance**: Corrected stale release and publishing guidance so downstream packagers can distinguish repository metadata from packages that have actually been submitted or published.
- **Compatibility**: No BRK1/BRK2 wire-format, public API, or runtime behavior changes are introduced by this maintenance release.

### Upgrade notes

- Source builds identify the project as 1.1.3 through CMake package metadata and CPack package versioning.
- Rebuild downstream packages from the v1.1.3 source tag after it is created; do not assume that package-manager registries have been updated by this repository change.
- The Homebrew formula checksum must be regenerated from the final v1.1.3 release archive before publishing the formula update.

---

## v1.1.2
Date: 2026-11-13

### Highlights

- **BRK2 parser hardening**: Added O(1) TLV duplicate detection and enforced the shared 4,096-field limit.
- **Broker client tracking**: Added O(1) active-client ID collision checks alongside the bounded source LRU.
- **Placement concurrency**: Reduced affinity-lock scope so queue-depth callbacks run outside the affinity mutex.
- **Adversarial benchmark accuracy**: Correlated response latency by BRK2 request ID and removed synthetic fallback measurements.
- **Protocol boundary coverage**: Added conformance tests for the BRK2 field-count ceiling.

---

## v1.0.0 (Historical)
Date: 2026-10-11

This release formalizes the project as a stable, production-facing GPU placement and broker interoperability library. It consolidates the public contract, expands the documentation set, and makes the CPU-only and GPU-capable operational modes clearer for both development and production users.

Highlights

- Stable placement policy: NVLink-aware home-GPU affinity, peer-aware rebalancing, and deterministic fallback behavior.
- Broker interoperability: BRK1/v1 protocol is the supported external contract for request/response dispatch and service integration.
- CPU-only compatibility: the broker service can run without CUDA for smoke tests, CI, and non-GPU environments.
- Packaging readiness: Conan, vcpkg, and Homebrew guidance is aligned with the release version and publish flow.
- Documentation refresh: installation, API usage, deployment, operations, protocol, and benchmarking references are aligned to the v1.0 release.

Notable changes

- README.md refreshed as a v1.0 overview with quick-start guidance and release-oriented documentation links.
- docs/LIBRARY.md updated with the stable public API model and the current broker and placement flow.
- docs/PUBLISHING.md updated to use version 1.0.0 package metadata and release checklists.
- DEPLOYMENT_GUIDE.md updated to reference the v1.0 operational model and CPU/GPU build modes.
- The broker protocol and execution boundary are documented as the default supported integration contract for external systems.

Upgrade notes

- v1.0 follows the stable public API surface used by the placement library and broker service; it is the recommended baseline for new integrations.
- Users migrating from earlier v0.3 docs or examples should treat the BRK1/v1 contract as the canonical interface and use the current release notes and docs as the source of truth.
- For packaging, update any downstream version references to 1.0.0 before publishing or tagging a release build.

How to get the release

- From source:

```bash
git clone https://github.com/rakshas-oss/overhauled.git
cd overhauled
git fetch --tags origin
git checkout v1
```

- Package managers: see packaging/README.md and docs/PUBLISHING.md for current packaging guidance.

Acknowledgements

Thanks to all contributors, operators, and benchmark participants who helped stabilize the v1.0 release. If you have questions, open an issue or use the repository discussion and operations guides.
