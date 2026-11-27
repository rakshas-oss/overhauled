# Overhauled package publishing

This guide distinguishes a published GitHub source release from independent package-manager channels. The latest published GitHub Release is v1.10.0 (2026-11-26); v1.11.0 is the current release candidate.

## Current distribution status

| Channel | Current state | Package |
|---|---|---|
| GitHub Releases | v1.10.0 published; v1.11.0 candidate | Source and release artifacts |
| CMake package | Project version 1.11.0 candidate | OverhauledTensorRuntime |
| Conan | Recipe version 1.11.0 candidate; no remote upload claimed | nvlink_placement/1.11.0 |
| Root vcpkg manifest | Version 1.11.0 candidate only | nvlink-placement |
| vcpkg Git registry | Port and baseline remain pinned to verified 1.6.0 archive; 1.11.0 registry update not prepared | nvlink-placement |
| Homebrew | Formula remains pinned to verified 1.6.0 source archive; no dedicated maintained tap | nvlink-placement |
| Conan Center Index | Not submitted; review and CLA required | nvlink_placement |
| Debian/RPM | CPack can generate local packages on Linux; no hosted apt/dnf repository | OverhauledTensorRuntime |

The v1.11.0 candidate metadata is not a release. Do not upload or advertise packages until the candidate passes CI, receives an approved immutable tag and has verified release checksums.

## vcpkg Git registry

The repository-hosted registry is independent of the root vcpkg manifest. The current registry port and baseline remain pinned to the last verified 1.6.0 archive. Updating root vcpkg.json to 1.11.0 does not change or publish that registry port.

A consumer's vcpkg-configuration.json should pin the registry to an immutable commit containing the port and version database. Do not substitute the main branch as a reproducible baseline. The registry port intentionally builds CPU-only and does not enable CUDA, TensorRT, Wasmtime or optional OpenSSL handoff.

Before updating the registry for v1.11.0:

1. Create and validate the immutable v1.11.0 source tag and GitHub Release.
2. Run the .github/workflows/release-source-digests.yml workflow with release_tag=v1.11.0 and preserve the generated SHA512/SHA256 values.
3. Verify the vcpkg port's CMake export/relocation layout and build an installed downstream consumer.
4. Update the port file, version database and baseline together in a separate reviewed change; never reuse the 1.6.0 checksum.

## Homebrew

The checked-in formula remains pinned to the immutable 1.6.0 source archive and its verified digest. Automated macOS installation checks remain paused because the broker server has a known macOS SDK std::chrono build mismatch. Do not update the formula to v1.11.0 until the build issue is fixed and the exact source archive SHA256 is verified.

There is no dedicated maintained third-party tap or Homebrew Core acceptance. A checked-in formula does not mean it is published in a tap.

## Conan

The checked-in Conan 2 recipe has candidate version 1.11.0. Validate it with:

~~~sh
conan profile detect --force
conan create . --build=missing
~~~

No Conan remote upload is claimed. Configure the intended remote and credentials separately and publish only the validated candidate after release approval. Conan Center Index submission is separate and requires its own review and contributor agreement.

## Debian and RPM

The CMake project enables CPack DEB/RPM generators on Linux. Build local packages with:

~~~sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BROKER=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=ON \
  -DENABLE_CUDA=OFF -DENABLE_TENSORRT=OFF \
  -DENABLE_WASM_RUNTIME=OFF -DENABLE_WASM_HANDOFF=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB -B packages
cpack --config build/CPackConfig.cmake -G RPM -B packages
~~~

These are local artifacts, not indexed apt/dnf repositories. Hosted repositories require signed metadata, publication infrastructure and maintenance.

## Security and release boundaries

- Package-manager validation defaults to CPU-only and does not establish physical NVLink, CUDA, TensorRT or Ethos-U85 functionality.
- Windows/MSVC native build is not a supported target. Portable package-consumer validation is restricted to the supported Linux/macOS lanes.
- No package registry credentials belong in source control. Use each registry's secrets, API tokens or trusted-publishing mechanisms.
- Do not advertise a package as published until it appears in the registry and a clean consumer installation succeeds.
