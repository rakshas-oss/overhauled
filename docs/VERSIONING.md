# Overhauled versioning

## Release state

- **Latest published GitHub release:** [v1.11.2](https://github.com/rakshas-oss/overhauled/releases/tag/v1.11.2) (published 2026-11-28). No binary assets are manually attached.
- **CMake project version:** 1.11.2.
- **Conan recipe version:** 1.11.2; remote upload is not claimed.
- **Root vcpkg manifest:** 1.11.2; the external vcpkg registry remains separately pinned to 1.6.0.
- **Published vcpkg registry baseline and Homebrew formula:** remain pinned to the last verified immutable source/archive baseline (1.6.0); no 1.11.2 registry port or formula update is included.
- **Package identity:** OverhauledTensorRuntime / nvlink_placement.
- **External package uploads:** no Conan remote upload, vcpkg registry publication, Homebrew update, Debian/RPM repository or other package-manager publication is implied by GitHub release metadata.

## Version assessment

The published v1.11.2 release synchronizes CMake, Conan and root vcpkg project metadata and refreshes release-facing documentation. No C++ implementation, public header, dependency, or protocol changes were included relative to v1.11.1. This was a metadata/documentation-only patch:

- Advance the CMake project version, Conan recipe version, and root vcpkg manifest together to 1.11.2.
- Refresh release/versioning/package documentation so the latest published tag and any future proposed version are clearly distinguished.
- Keep the vcpkg Git registry and Homebrew formula pinned to the verified 1.6.0 archive. The root manifest is not itself a registry publication.
- Preserve all BRK1, BRK2, WSM1, NXR1 and MED1 wire identities and all library behavior.

This patch increment is not a feature release and does not add a new crawler API, broker operation, GPU/NPU path, or automatic YuKKi-OS integration. YuKKi-OS consumes Overhauled through explicitly configured compatible endpoints; the two repositories remain separately versioned.

## Compatibility and security boundaries

- The standalone crawler_metrics library accepts caller-normalized page observations. It does not fetch URLs, parse HTML, authenticate sources, or add broker routes.
- The multivariate_stats library accepts normalized numeric observations only. It does not handle raw page bodies, establish provenance or authorization, or add a BRK operation.
- Pairwise sample counts can differ between matrix cells and must accompany covariance/correlation values.
- Accumulators are not internally synchronized. Use one accumulator per worker/NUMA shard and merge at a controlled barrier.
- Existing BRK1/BRK2/WSM1/NXR1/MED1 identities remain unchanged.
- Broker framing is not transport authentication. The documented mTLS profile uses an external sidecar, not a native TLS listener.
- The optional replay journal persists accepted nonces, not staged artifacts, prepared deployments, or active runtime state.
- The optional Wasmtime C API composition runtime shares a Store/resource domain; it is not a multi-tenant sandbox.
- CUDA, TensorRT, Ethos-U85 and NVLink hardware features remain separately gated. CPU-only CI does not validate physical accelerator hardware.
- External vcpkg/Homebrew recipes remain pinned to the verified 1.6.0 source archive until exact source checksums and downstream package validation are available.

## Release verification and future distribution process

The v1.11.2 tag and GitHub Release were published on 2026-11-28. No binary assets are manually attached. The CMake, Conan and root vcpkg metadata are versioned at 1.11.2; this does not publish to external package managers. For a future external package update, use the following validation steps:

1. Re-run the native Overhauled CI suite against the exact tagged source, including compiler warnings, unit/CTest, BRK2 conformance, worker-fabric saturation and paired YuKKi-OS interoperability.
2. Require ASan/UBSan validation and package-consumer smoke tests against the 1.11.2 CMake install/export surface.
3. Validate the Conan recipe with a local conan create . --build=missing against the published 1.11.2 metadata; do not upload to a remote.
4. Validate root vcpkg manifest syntax and ensure future changes do not silently alter the published vcpkg registry baseline or Homebrew formula.
5. Confirm CMake, Conan and root vcpkg version fields all equal 1.11.2.
6. Confirm no runtime implementation, public headers, dependencies, or protocol identities changed.
7. For a future external-registry/formula update, calculate checksums from the immutable v1.11.2 source archive and validate downstream consumers before changing the separately maintained registry/formula files.

## Published release history

- v1.11.2 — published 2026-11-28; CMake, Conan and root vcpkg metadata synchronization plus documentation refresh. No manually attached binary assets; no external registry upload implied.
- v1.11.1 — published 2026-11-27; metadata synchronization and audience-focused documentation. No binary assets attached; external package registries remain on their separately verified baselines.
- v1.11.0 — published 2026-11-26; fixed-memory online pairwise covariance and Pearson correlation for up to 32 numeric dimensions, with pairwise sample counts.
- v1.10.0 — published 2026-11-26; standalone CPU-only crawler_metrics accumulator for normalized page observations.
- v1.9.0 — published 2026-11-26; bounded broker sessions, loopback-only sidecar routing and optional replay-nonce journaling.
