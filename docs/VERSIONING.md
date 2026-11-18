# Overhauled versioning

## Current release candidate

- **Proposed version:** `1.1.4`
- **CMake project version:** `1.1.4`
- **Conan recipe version:** `1.1.4`
- **Runtime/package identity:** `OverhauledTensorRuntime` / `nvlink_placement`
- **Release state:** candidate; no v1.1.4 tag or release archive has been published

## Version policy

- The v1.1.4 patch release records architecture, heuristic and sysadmin
  documentation corrections plus CMake/Conan version metadata alignment.
- This candidate intentionally does not change BRK1/BRK2 or WSM1 wire formats,
  public C++ interfaces, backend-selection rules, or broker execution behavior.
- Keep YuKKi-OS as the separate Wasmtime guest/runtime and composition host.
  Overhauled owns broker framing, task dispatch, GPU placement and its
  configured CPU/GPU/NPU executor boundary.
- Do not publish a tag or release archive until the release PR is merged,
  required CI passes, and the intended release commit is confirmed.
- Do not claim that Conan Center, vcpkg, Homebrew, apt or dnf has been updated
  merely because this repository's manifests or docs have changed.

## Package-manager publication order

- CMake and Conan are aligned to the candidate version in this branch.
- The vcpkg manifest and source port pin remain at the last published source
  until the v1.1.4 tag exists. After tagging, update the manifest and portfile
  together and calculate SHA512 from the exact v1.1.4 archive.
- The Homebrew formula currently points at an older archive. Update its URL,
  version and SHA256 only after downloading the final v1.1.4 release archive.
- Submit each external package-manager PR separately and verify the registry
  result before describing that ecosystem as published.

## Previous published release

- `v1.1.3` — last published release; CMake, Conan and release metadata
  synchronization with the earlier broker hardening and documentation work.

