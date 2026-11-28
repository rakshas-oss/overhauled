# Overhauled versioning

## Release state

- **Latest published GitHub release:** v1.11.1 (published 2026-11-27). The release has no attached binary assets.
- **Next patch release candidate:** v1.11.2 (not tagged or published).
- **CMake project version:** 1.11.2 in this candidate.
- **Conan recipe version:** 1.11.2 in this candidate.
- **Root vcpkg manifest:** 1.11.2 in this candidate.
- **Published vcpkg registry baseline and Homebrew formula:** remain pinned to the last verified immutable source/archive baseline (1.6.0); no 1.11.2 registry port or formula update is included.
- **Package identity:** OverhauledTensorRuntime / nvlink_placement.
- **External package uploads:** no Conan remote upload, vcpkg registry publication, Homebrew update, Debian/RPM repository or other package-manager publication is implied by GitHub release metadata.

## Version assessment

The published v1.11.1 release synchronized project metadata. The subsequent audience-focused README updates are documentation-only. Since that tag, no C++ implementation, public header, dependency, or protocol changes have landed in Overhauled. The v1.11.2 patch candidate is therefore a metadata/documentation-only increment:

- Advance the CMake project version, Conan recipe version, and root vcpkg manifest together to 1.11.2.
- Refresh release/versioning/package documentation so the latest published tag and the next candidate are clearly distinguished.
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

## Release gate for 1.11.2

Before tagging or publishing:

1. Run the full native Overhauled CI suite on the exact candidate, including compiler warnings, unit/CTest, BRK2 conformance, worker-fabric saturation and paired YuKKi-OS interoperability.
2. Pass ASan/UBSan validation and package-consumer smoke tests using the candidate CMake install/export surface.
3. Validate the Conan recipe with a local conan create . --build=missing against the candidate metadata; do not upload to a remote.
4. Validate root vcpkg manifest syntax and ensure the candidate does not silently change the published vcpkg registry baseline or Homebrew formula.
5. Confirm CMake, Conan and vcpkg version fields all equal 1.11.2.
6. Confirm no runtime implementation, public headers, dependencies, or protocol identities changed.
7. Only after required checks pass and the release commit is approved, create the v1.11.2 tag and GitHub Release. Calculate source checksums before any later external-registry/formula updates.

## Published release history

- v1.11.1 — published 2026-11-27; metadata synchronization and audience-focused documentation. No binary assets attached; external package registries remain on their separately verified baselines.
- v1.11.0 — published 2026-11-26; fixed-memory online pairwise covariance and Pearson correlation for up to 32 numeric dimensions, with pairwise sample counts.
- v1.10.0 — published 2026-11-26; standalone CPU-only crawler_metrics accumulator for normalized page observations.
- v1.9.0 — published 2026-11-26; bounded broker sessions, loopback-only sidecar routing and optional replay-nonce journaling.
