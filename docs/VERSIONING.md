# Overhauled versioning

## Current release candidate

- **Proposed version:** `1.1.5`
- **CMake project version:** `1.1.5`
- **Conan recipe version:** `1.1.5`
- **vcpkg manifest version:** `1.1.5`
- **Runtime/package identity:** `OverhauledTensorRuntime` / `nvlink_placement`
- **Release state:** candidate; the v1.1.5 tag and release archive are pending

## Version assessment

The patch increment is based on the merged work after published v1.1.4:

- PR #49 hardens dataset manifest parsing, bounds unsigned sequence/timestamp
  fields, protects dataset-root path resolution, validates BRK2 response
  correlation/status/backend telemetry, and reports actual backend/fallback
  counts in the benchmark output.
- PR #50 clarifies the browser-WASM-to-BRK2 benchmark bridge and distinguishes
  direct benchmark inference from the WSM1 guest lifecycle path.
- PR #51 adds full-colour system diagrams and makes the YuKKi-OS versus
  Overhauled responsibility boundary explicit.

These changes improve qualification tools, validation and documentation.
They do not add a new public C++ API, change backend dispatch semantics, or
revise BRK1/BRK2/WSM1 wire formats. A patch increment to 1.1.5 is therefore
more appropriate than a minor or major increment.

## Release policy

- v1.1.4 is the latest published Overhauled release.
- Do not create or publish v1.1.5 until its release PR is merged, required CI
  passes, and the intended release commit is confirmed.
- Updating this repository does not publish Conan Center, vcpkg, Homebrew,
  apt or dnf packages. Verify each external registry separately.
- The vcpkg manifest is set to candidate version 1.1.5, but the portfile's
  source reference/checksum must be refreshed from the final tagged source
  archive before registry submission.
- The checked-in Homebrew formula still references v1.1.1. Do not change its
  archive URL or SHA-256 without obtaining the exact v1.1.5 release archive.
- Keep YuKKi-OS as the separate Wasmtime guest/runtime and composition host.
  Overhauled owns broker framing, task dispatch, GPU placement and its
  configured CPU/GPU/NPU executor boundary.

## Published release history

- `v1.1.4` — published 2026-11-18; architecture/heuristic and release
  metadata alignment.
- `v1.1.3` — published 2026-11-18; release metadata and prior broker hardening.
- `v1.1.2` — prior published release.

