# TRL-5 Adversarial Validation Candidate

## Purpose

This plan turns the merged broker hardening and BRK/NXR1 interoperability work into a reproducible validation candidate. The intended claim is **TRL-5 evidence preparation**: integrated components are validated in a relevant, controlled environment. A successful software test campaign does not by itself establish TRL-5; the evidence must include a representative deployment configuration, measured results, reviewed failures, and repeatable procedures. TRL-6 requires a representative prototype demonstration in a relevant operating environment.

## System boundary

Overhauled is the execution and placement tier. The candidate includes:

- BRK1/v1 and BRK2/v2 framing and schema validation on the broker listener.
- NXR1 v1 decoding/encoding and the shared Rust/C++ golden vector.
- Bounded client/source tracking, request framing, deadlines, and broker lifecycle.
- CPU baseline, simulated NPU path, optional CUDA/TensorRT paths, and opt-in Linux Ethos-U85 DRM path.
- The YuKKi-OS client/adapter as the external control-plane integration partner.

The broker TCP protocol is not authentication or encryption. Candidate network tests must use an isolated test network or an explicitly secured test boundary. Do not describe the broker's transport as authenticated merely because the upstream mesh is authenticated.

## Validation rules

1. Every failure is reproducible from a recorded seed, input artifact, build identifier, and test command.
2. Tests must assert protocol and runtime invariants; they must not be weakened or silently skipped to obtain a pass.
3. A timeout test measures elapsed time and terminal task state, not just whether a timeout value parses.
4. Simulated NPU success is labelled simulation. It is not evidence of physical accelerator execution.
5. Record both successful requests and rejected/faulted requests. No crash, hang, unbounded growth, cross-client response mix-up, or silent corruption is acceptable.
6. Establish performance baselines on named hardware before setting absolute latency/throughput claims.

## Test campaign and gates

### Gate A — reproducible build and software baseline

- Build the CPU-only broker in a clean environment with pinned compiler/dependency versions.
- Run the full CTest suite and record compiler, CMake, OS, architecture, commit SHA, and test logs.
- Run ASan and UBSan builds; run TSan on supported concurrency targets.
- Run static analysis and dependency/security audits already configured by the repository.
- Preserve a clean baseline report before enabling optional accelerator features.

**Pass:** clean build; all required tests pass; sanitizer findings are triaged and no unresolved critical memory-safety or data-race defect remains.

### Gate B — protocol and codec adversarial validation

Exercise BRK1, BRK2, NXR1 and WSM1 boundaries, including:

- Zero, minimum, maximum, maximum-plus-one, truncated, and overlong frame lengths.
- Partial prefix/body reads, multiple frames in one connection, trailing bytes, and slow/incomplete frames.
- BRK2 invalid magic/version, reserved bits/bytes, invalid status values, malformed TLV lengths, duplicate singular fields, unknown optional fields, and unknown critical fields.
- Correlation-ID mismatch, invalid enum values, missing required fields, and malformed nested payloads.
- NXR1 canonical field order, 81-byte header, little-/big-endian conversion, truncated payload, length mismatch, and shared golden-vector parity.
- WSM1 task and lifecycle payload lengths, invalid message types, malformed buffer descriptors, and request/response ID mismatch.

Run at least 10,000 deterministic mutations per pull request and a longer nightly campaign (target at least 1,000,000 mutations across seeded corpus inputs). Preserve every minimized crash or semantic mismatch as a regression fixture. Use coverage-guided fuzzing where supported; deterministic mutation tests remain mandatory on all CI platforms.

**Pass:** no crash, panic, out-of-bounds access, unbounded allocation, decoder disagreement, or accepted malformed input outside the written protocol contract. Any unresolved BRK1 reserved-byte or response-status behavior is a release blocker until specified and tested.

### Gate C — broker resource, timeout, and concurrency resilience

Test connection and workload levels of 1, 8, 32, and 128 concurrent clients, plus the configured per-client in-flight limit and boundary values. Include:

- Client/source LRU churn beyond its configured capacity; verify evicted client resources are released and identifiers do not collide or wrap.
- Fragmented writes, stalled senders, disconnect before/during compute, and reconnect storms.
- Pipelined requests with randomized response timing; verify responses remain correlated and writes are serialized correctly.
- Deadline races at below/equal/above simulated execution latency; assert elapsed-time bounds and terminal status.
- Saturated queues and worker pools; verify explicit backpressure/rejection rather than unbounded memory or thread growth.
- Graceful shutdown while idle, under active load, and with a deliberately delayed worker; measure drain time and document the behavior of work that cannot be preempted.
- GPU/NPU unavailable, initialization failure, and device saturation; verify only policy-approved CPU fallback and verify the reported backend.

**Pass:** no cross-client data leakage, duplicate completion, deadlock, unbounded resource growth, or silent task loss. Deadline behavior meets the configured contract. Cancellation/preemption limitations are documented and not represented as guaranteed.

### Gate D — execution-path and interoperability validation

- CPU-only broker: canonical correctness path.
- Simulated NPU: exercise dispatch and verify the response identifies the simulated path accurately.
- CPU fallback: test both fallback-permitted and fallback-forbidden requests.
- CUDA/TensorRT: test only on named compatible hardware/software stacks; record driver, toolkit, runtime, device, model, and precision.
- Ethos-U85: run simulator/conformance checks separately from a physical board test. A physical test requires a supported Linux DRM UAPI, compatible Vela bundle, target board, and observed completed inference reported as backend `npu`.
- Run the live YuKKi-OS ↔ Overhauled BRK1 and BRK2 integration suite, including NXR1 shared golden-vector parity and WSM1 lifecycle operations.

**Pass:** request intent, actual backend, result status, task identity, and payload are consistent end-to-end. A simulated or compile-only result is never counted as hardware validation.

### Gate E — soak, recovery, and operational evidence

- Run a 24-hour CPU-only soak with representative mixed valid/invalid traffic.
- Run a shorter high-load soak on each optional accelerator profile that is actually available.
- Capture RSS, heap/allocator or process memory, open file descriptors, thread count, queue depth, request rate, rejection rate, p50/p95/p99 latency, timeout count, and backend distribution.
- Restart the broker during load; verify bounded recovery and client-visible failure semantics.
- Archive logs, metrics, test seeds, minimized reproducers, environment manifests, and exact commands.

**Pass:** no leak trend, unexplained crash, silent corruption, or unrecovered deadlock. Performance regressions are compared against a same-hardware baseline; investigate any p95/p99 regression greater than 10% or throughput regression greater than 10% before acceptance. These are investigation thresholds, not universal performance claims.

## Required evidence package

Store a dated candidate report containing:

- Candidate name/version, commit SHAs for both repositories, build provenance, toolchain and dependency lock state.
- Hardware/software inventory and network topology.
- Test matrix, commands, seeds, durations, raw logs, coverage summaries, and minimized regression corpus.
- Per-gate pass/fail/blocked status, defect links, severity, owner, and retest evidence.
- Benchmark baseline and candidate comparison with sample counts and variance.
- Explicit exclusions: unsupported hardware/platform combinations, no broker-native TLS/authentication, no guaranteed remote cancellation/preemption, and any untested physical NPU path.
- Sign-off from engineering, security, and test owners.

## Candidate result template

| Gate | Result | Evidence link | Open blockers |
|---|---|---|---|
| A — Build/sanitizers | Not run | | |
| B — Protocol/fuzz | Not run | | |
| C — Resource/concurrency | Not run | | |
| D — Execution/interop | Not run | | |
| E — Soak/recovery | Not run | | |
| Relevant-environment demonstration | Not run | | |

Do not mark the candidate TRL-5 validated until all required gates have evidence and the relevant-environment demonstration has been reviewed. Record TRL-6 as a separate future gate.

## Cross-repository plan

The matching YuKKi-OS campaign is documented in [TRL-5 Adversarial Validation Candidate](https://github.com/rakshas-oss/YuKKi-OS/blob/main/docs/TRL5_ADVERSARIAL_VALIDATION.md). Both plans must be executed against mutually recorded commit SHAs; testing either repository in isolation is not end-to-end evidence.
