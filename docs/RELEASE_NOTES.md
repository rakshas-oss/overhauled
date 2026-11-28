## v1.11.2 (patch release candidate; not published)
Date: 2026-11-28

### Highlights

- Advance CMake, Conan and root vcpkg project metadata together from 1.11.1 to 1.11.2.
- Update versioning, package distribution, publishing and feature documentation to distinguish the published v1.11.1 release from this candidate.
- Preserve the vcpkg registry and Homebrew formula at their separately verified 1.6.0 source/archive baseline.

### Compatibility and limitations

- This is a metadata/documentation-only patch candidate. No C++ implementation, public header, dependency, or protocol behavior changes are included.
- Existing BRK1/BRK2/WSM1/NXR1/MED1 wire identities remain unchanged.
- No external vcpkg registry, Homebrew formula, Conan remote, Debian/RPM repository, or other package publication is implied.
- No v1.11.2 tag or GitHub Release exists until the release gate passes.

### Release gate

Run the native, package-consumer, paired YuKKi interoperability, BRK2, worker-fabric, and sanitizer checks on the exact candidate commit. Do not tag or publish v1.11.2 until all required checks pass.

---

## v1.11.1 (published 2026-11-27)
Date: 2026-11-27

### Highlights

- Synchronize the CMake project, Conan recipe and root vcpkg manifest metadata on 1.11.1.
- Subsequent README audience and package-boundary wording changes on main were documentation-only and are not a runtime feature of the v1.11.1 tag.

### Compatibility and limitations

- This is a metadata/documentation-only patch; no C++ implementation, public header, dependency or protocol change is included.
- Existing BRK1/BRK2/WSM1/NXR1/MED1 wire identities remain unchanged.
- The external vcpkg registry and Homebrew formula remain pinned to the verified 1.6.0 source archive. No Conan remote upload, registry update or Homebrew publication is implied.
- The v1.11.1 GitHub Release has no attached binary assets.

---

## v1.11.0 (published 2026-11-26)
Date: 2026-11-26

### Highlights

- Add the standalone C++17 multivariate_stats library and Overhauled::multivariate_stats CMake target with the public multivariate_stats.h header.
- Compute online pairwise covariance and Pearson correlation for up to 32 dimensions using fixed-size accumulator state.
- Preserve missing observations through pairwise deletion and expose per-pair sample counts; missing values are not converted to zero.
- Support independent worker/NUMA shard accumulators and controlled merges without a shared hot lock.
- Reject invalid dimensions, masks, non-finite present values and overflow without partial mutation.

### Compatibility and limitations

- The component accepts normalized numeric observations only; it does not ingest sockets, parse raw crawler data, authenticate sources or add a broker protocol operation.
- Pair counts can differ between matrix cells and must be considered by consumers.
- The library is CPU-only. No GPU/NPU dispatch, automatic YuKKi-OS integration or performance improvement is claimed.
- Existing BRK1/BRK2/WSM1/NXR1/MED1 protocol identities are unchanged.

### Release metadata and validation

CMake, Conan and the root vcpkg manifest were advanced to 1.11.0 for the published GitHub Release. The v1.11.0 tag and GitHub Release were published on 2026-11-26; the release record has no attached binary assets. The published vcpkg registry port and Homebrew formula remain pinned to the verified 1.6.0 source archive. No Conan remote upload or external registry publication is claimed.

---

## v1.10.0 (published 2026-11-26)
Date: 2026-11-26

### Highlights

- Add the standalone C++17 `crawler_metrics` library and `Overhauled::crawler_metrics` target with an installed public header.
- Aggregate normalized page observations: status success/failure, document bytes, link totals, fetch/extraction duration statistics and bounded histogram percentile upper bounds.
- Support per-worker/per-NUMA-shard accumulators with barrier-based merging and fixed memory use.
- Reject counter overflow and self-merge without mutating accumulator state.
- Pair with the YuKKi-OS v6.21.0 WASM Crawler Fabric by mapping its page-level numeric output into `PageObservation` on the caller side.

### Compatibility and limitations

- The metrics library is CPU-only, accepts normalized numeric observations and contains no crawler networking or HTML parsing.
- No BRK1/BRK2 message or broker listener route is added. The accumulator is not automatically wired to YuKKi-OS.
- Raw HTML is not passed into Overhauled; no GPU/NPU crawler acceleration or measured throughput improvement is claimed.
- P50/P95/P99 values are upper bounds from fixed log2 buckets, not exact quantiles.
- Existing BRK1/BRK2/WSM1/NXR1/MED1 protocol identities are unchanged.

### Release metadata and validation

CMake, Conan and the root vcpkg manifest were advanced to 1.10.0 for the published GitHub Release. The external vcpkg registry and Homebrew formula remain pinned to the last verified immutable source/archive baseline (1.6.0); no 1.10.0 external registry, formula or Conan remote publication is implied.

---

## v1.9.0 (published 2026-11-26)
Date: 2026-11-26

### Highlights

- Bound broker client sessions to a hard ceiling of 1,024 and expose `--max-clients`; each admitted session still owns one blocking I/O thread.
- Add a configurable IPv4 listener address and `--require-mtls-proxy`, which rejects non-loopback bind addresses to keep the plaintext broker listener behind a local TLS terminator.
- Provide an stunnel mTLS 1.3 client/server profile with peer-certificate verification, server-chain/hostname checks and loopback forwarding.
- Add optional append-only ReplayGuard journaling; accepted nonces are fsync'd before success and recovered across a single-process restart. The journal has a 64 MiB cap and fails closed on corruption or persistence errors.
- Implement platform-specific journal file I/O and add tests for listener limits, strict binding and replay after restart.

### Compatibility, security and limitations

- Preserve BRK1/BRK2, WSM1, NXR1 and MED1 wire identities; no framing change is introduced.
- TLS is terminated by the documented sidecar; there is no native TLS broker listener in this candidate.
- The transport certificate is not yet bound to the signed deployment-sender identity.
- The optional journal persists accepted nonces only. Prepared artifact bytes, active-release metadata and runtime state remain process-local; multi-process/restart-safe deployment still needs a transactional staging and activation store.
- Socket I/O remains thread-per-client with an enforced 1,024-session ceiling, not the planned fixed event-driven I/O worker pool.
- Untrusted stream access remains separate from executable-artifact publisher authorization and retains its own resource limits.

### Release metadata

CMake, Conan and the root vcpkg manifest advanced to 1.9.0. The v1.9.0 tag and GitHub Release were published on 2026-11-26. The external vcpkg registry and Homebrew formula remain pinned to the last verified source/archive baseline until exact 1.9.0 source checksums and registry updates are completed; GitHub Release publication does not imply external package uploads.

---

## v1.8.0 (published 2026-11-24)
Date: 2026-11-23

### Highlights

- Honor an explicit BRK2 `device_preference=gpu` for bounded workloads even when the payload is below the automatic GPU offload threshold.
- Keep automatic threshold-based GPU selection unchanged when no explicit GPU preference is supplied.
- Preserve forced-CPU, exact-CPU/verification-sensitive policy and real GPU availability as higher-priority constraints; report the backend actually selected rather than treating the preference as proof of acceleration.
- Keep CPU fallback authoritative when the requested GPU is unavailable.

### Compatibility and validation

- No BRK1/BRK2 wire-format change; this clarifies and enforces existing BRK2 field 10 semantics.
- Validate both available-GPU dispatch for sub-threshold payloads and CPU selection when no GPU is available.
- CUDA/NVLink hardware validation remains opt-in and requires a capable runner; ordinary CPU CI does not claim hardware coverage.

### Release metadata

CMake, Conan and the root vcpkg manifest were advanced to 1.8.0. The 1.8.0 Git tag and GitHub Release were published on 2026-11-24. The vcpkg registry port and Homebrew formula remain on their last verified source/archive baseline until exact source checksums are computed and those registries are updated; GitHub Release publication does not itself prove external package publication.

---

## v1.7.0 (published 2026-11-22)
Date: 2026-11-22

### Highlights

- Complete the opt-in authenticated-WASM receiver state machine: separate deployment sender and artifact publisher trust, expiry/nonce/replay checks, signed prepare decisions and activation receipts.
- Bound concurrent preparations, total staged bytes, module/chunk sizes, global/per-sender message rates and global/per-sender ingress-byte rates.
- Add a Wasmtime runtime bridge that compiles the verified staged module set and publishes it only through the configured atomic activation callback. Failed preparation or activation preserves the previous active release.
- Keep untrusted NXR1, MED1 and media/stream-relay data paths available without WASM publisher authorization, under independent stream and gateway resource/rate/backpressure limits.

### Compatibility and security boundary

- Existing BRK1/BRK2, WSM1, NXR1 and MED1 framing identities remain unchanged; executable handoff is a separate application protocol.
- The receiver is a library API, not a handler on the ordinary BRK1/BRK2 TCP listener.
- A production caller must provide an authenticated protected transport (mTLS 1.3 or equivalent), bind transport identity to the signed deployment sender, and provide durable replay/staging for restart-safe deployment semantics.
- The current shared-Store Wasmtime embedding is not a multi-tenant sandbox.
- Application-level rate/byte limits do not replace upstream controls against volumetric DDoS.

### Release metadata and validation

The 1.7.0 Git tag and GitHub Release were published on 2026-11-22. External package-manager registry publication is separate and is not implied by the GitHub Release.

---
## v1.6.0 (published 2026-11-22)
Date: 2026-11-22

### Highlights

- **Authenticated-WASM receiver primitives:** add opt-in OpenSSL EVP Ed25519 signature verification over caller-supplied RFC 8785 canonical bytes, with out-of-band trust entries bound to issuer, subject and audience.
- **Artifact integrity:** add SHA-256 helpers, bounded process-local replay protection, sequential bounded chunk staging, per-chunk digest validation and final artifact digest verification.
- **Independent build options:** `ENABLE_WASM_HANDOFF=ON` enables the new receiver primitives and requires OpenSSL Crypto 1.1.1 or newer. `ENABLE_WASM_RUNTIME` remains an independent optional Wasmtime feature; the default build remains unchanged.
- **Wasmtime v51 correction:** carry forward the C API and linker-validation fixes previously documented in the v1.5.1 patch candidate.

### Explicit implementation boundary

This published release does not provide a network-facing authenticated BRK2 handoff handler. It does not establish mTLS or authenticated peer identity, decode and canonicalize the full handoff schema, verify artifact publisher provenance, persist replay state across processes, issue signed prepare decisions or execution receipts, select/admit a runtime backend, or activate a release. Do not advertise the receiver as a deployable authenticated handoff service.

The verifier expects RFC 8785 canonical bytes from its caller. Replay protection is process-local. The Wasmtime embedding continues to use a shared Store/resource domain and is not a complete tenant sandbox.

### Validation and follow-up verification

The GitHub release tag `1.6.0` points to commit
`326609fa7ad7a21b3fe62a8c29743b0a40eac57d`. On candidate commit
`4bcba05c6381bff6cd0fb9c8ec28495944ba003a`, the seven reported release PR
workflow runs completed successfully: package consumer smoke, native CI,
Wasmtime v51 compile/runtime, paired YuKKi-OS interoperability, BRK2 C++
protocol tests, ASan/UBSan, Ethos-U85 Linux build, and mixed-class saturation
validation. The mixed-class A/B benchmark job itself was skipped.

**Post-publication test gap:** the original release workflows did not configure
`ENABLE_WASM_HANDOFF=ON`, so they did not execute the optional receiver test
target. A dedicated OpenSSL-enabled CMake/CTest lane is being added to the
primary CI workflow. Its result must be green before claiming the optional
handoff primitives passed feature-specific validation. The GitHub release is
already published; this follow-up does not retroactively change the original
release CI record.

---

## v1.5.1 (patch release candidate; tag pending)
Date: 2026-11-21

### Highlights

- **Wasmtime v51 C API build correction:** replace unavailable `wasm_message_new_empty` / `wasm_message_delete` calls with the supported `wasm_name_t` buffer lifecycle and remove invalid deletion of an immediate scalar `wasmtime_val_t`.
- **Linker validation test correction:** Wasmtime validates import signatures while registering the consumer module, so incompatible imports are now asserted to fail at registration rather than instantiation.
- **Release scope:** no BRK1/BRK2/WSM1 wire changes and no change to the default build with `ENABLE_WASM_RUNTIME=OFF`.

### Validation

- The published v1.5.0 tag was created before the correction and its Wasmtime-enabled build failed with the unavailable C API symbols.
- The correction and release-metadata update are merged on main. On merge commit `bacb67ec5918697ffabd0e533b1c403f85105a1a`, all 8 reported GitHub Actions checks passed: paired repository release gate, Wasmtime v51 compile/runtime composition, BRK2 codec/mutation, paired YuKKi-OS interoperability, ASan/UBSan validation, and Ubuntu/Windows installed CMake package consumers.
- The Windows check is package-consumer validation, not native Windows/MSVC runtime support. The Wasmtime lane validates the documented Linux x86_64 v51.0.0 target.
- v1.5.1 remains a candidate until the exact intended release/tag commit and release assets are verified. Do not describe it as published before that verification.

---

## v1.5.0 (published 2026-11-21)
Date: 2026-11-21

### Highlights

- **Optional native Wasmtime runtime composition:** compile core-Wasm modules and register them in a linker under module namespaces; dependent modules resolve imports at instantiation. This is runtime composition, not static binary merging.
- **Retained instances and initial invocation API:** expose a narrow `() -> i32` export call with explicit error handling for missing exports, traps and incompatible results.
- **Cross-module test fixture:** provider/consumer modules exercise import resolution and verify the returned value. The Wasmtime-enabled CI lane must pass before release.

### Security and compatibility boundary

- `ENABLE_WASM_RUNTIME` is opt-in; the default native build remains unchanged.
- Registered modules and instances share one Wasmtime Store and resource domain. This is not per-module or tenant isolation.
- Fuel metering, epoch interruption, WASI capability grants, artifact signing, cache eviction and task-level instance teardown are not yet complete. Do not expose this initial embedding as a complete untrusted-workload sandbox.
- Existing BRK1/BRK2/WSM1 envelope identities are unchanged.

### Release gate

Require default native CI, the pinned Wasmtime-enabled compile/runtime test, sanitizer, package-consumer and paired YuKKi interoperability checks on the exact release candidate. Do not tag or publish until all required checks pass.

---

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
Date: 2026-11-21

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
