# Overhauled package distribution

Overhauled is a C++17 CMake library and optional broker service. Its primary
package-manager target is the CMake package, not a prebuilt platform-specific
binary.

> **Release state (2026-11-27):** v1.10.0 is the latest published GitHub release;
> v1.11.0 is a release candidate. CMake, Conan and the root vcpkg manifest
> carry candidate metadata; the vcpkg registry and Homebrew formula remain
> pinned to the verified 1.6.0 source archive pending a reviewed checksum update.

## Recommended channels

| Channel | Intended use | Current state |
|---|---|---|
| vcpkg Git registry | C++ library and broker, CPU-only baseline | The published registry port remains pinned to the last verified immutable source/archive baseline (1.6.0). The root vcpkg manifest is 1.11.0 in the candidate, but no 1.11.0 registry port or baseline update is included. Automated vcpkg validation remains paused because the port's CMake export layout is not CI-validated. |
| Homebrew formula | macOS/Linux developer installation | The checked-in formula remains pinned to its last verified immutable source archive (1.6.0). Do not advance it to 1.11.0 until the immutable tag archive SHA256 is calculated and the known macOS `std::chrono` build issue is resolved; automated Homebrew validation remains paused. |
| Conan | Existing Conan consumers | The Conan 2 recipe version is 1.11.0 in this release candidate. Run `conan create . --build=missing` against the candidate; no remote upload is claimed. |
| Conan Center Index | Public curated Conan package | Not submitted; CCI reviews project maturity, usage and maintenance. An upstream PR is a separate contribution |
| Debian/RPM | System package installation | CPack emits native packages when built on Linux; no hosted apt/dnf repository is currently published |

## CI boundary: packages versus physical NVLink hardware

Package-manager validation is deliberately hardware-independent. The vcpkg
port, Homebrew formula, Conan recipe, and installed-CMake consumer builds
disable CUDA/TensorRT or default CUDA off. They validate compilation,
installation, exported CMake targets, and downstream linking; they do not
claim to validate a physical NVLink fabric.

The ordinary pull-request CI follows the same rule: CUDA/TensorRT are disabled
for CPU conformance, sanitizers, WASM, and package-consumer checks. Synthetic
GPU topologies remain suitable for deterministic scheduler and dispatch
tests, but they are not evidence that a physical NVLink connection works.

Physical validation is isolated in the manually dispatched
`.github/workflows/nvlink-hardware-validation.yml` workflow. It requires an
administrator-provisioned self-hosted Linux runner with these labels:

```text
self-hosted, linux, x64, nvlink
```

That runner must have the NVIDIA driver, `nvidia-smi`, the CUDA toolkit
(`nvcc`), at least two visible CUDA GPUs, and at least one physical NVLink
connection between them. Dispatch the workflow and explicitly choose
`run`; the default `no` selection does not schedule a job. The test verifies
the reported system topology, builds the opt-in
`nvlink_hardware_smoke_test`, checks that Overhauled detects an NVLink peer
pair, and exercises CUDA peer-access enablement.

The CMake option `ENABLE_NVLINK_HARDWARE_TESTS` defaults to `OFF`. Enabling it
requires both `BUILD_TESTING=ON` and a detected CUDA compiler/toolkit. The
hardware test is labelled `hardware;cuda;nvlink` and is not part of standard
CTest or any package-manager publication gate. No GitHub-hosted runner needs
physical NVLink access, and absence of an NVLink runner does not block
portable package validation.

## vcpkg Git registry

The repository-hosted Git registry provides a versioned port for `nvlink-placement`.
Consumers configure the repository as an additional Git registry and pin the
registry baseline to a commit that contains the port and versions database. The
root `vcpkg.json` manifest is a separate local manifest; bumping it does not
publish a new registry port.

A consumer's `vcpkg-configuration.json` should include an entry like this,
The registry files were merged in commit `fd69f5af7b60a6c542f198edc78d87aa20c41d94`; use that immutable commit as the registry baseline:

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "YOUR_VCPKG_BASELINE_COMMIT"
  },
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/rakshas-oss/overhauled",
      "reference": "main",
      "baseline": "fd69f5af7b60a6c542f198edc78d87aa20c41d94",
      "packages": ["nvlink-placement"]
    }
  ]
}
```

Then install with:

```sh
vcpkg install nvlink-placement
```

The port intentionally builds the CPU-only default. CUDA/TensorRT and the
optional Ethos-U85 executor depend on system/toolchain-specific prerequisites
and are not silently enabled by the portable registry build. The vcpkg CI
consumer job is currently paused: the package installs, but its exported
CMake files are not yet relocated to vcpkg's canonical share directory.

## Homebrew

The checked-in formula still targets its verified immutable 1.6.0 source tag with CUDA and TensorRT
disabled by default. It remains pinned there; the 1.11.0 candidate is not an update to the formula. Its automated macOS installation check is currently
paused because the broker server does not compile on the current macOS SDK;
the failure is a C++ chrono type mismatch, not missing NVLink hardware.
Do not treat the formula as a verified macOS installation until that build
issue is resolved. Until a dedicated tap repository is created, consumers
can try the maintained formula directly from its raw GitHub URL:

```sh
brew install https://raw.githubusercontent.com/rakshas-oss/overhauled/main/Formula/nvlink-placement.rb
```

Homebrew Core inclusion is not guaranteed: it requires reproducible source
archives, supported build/test coverage and project maintenance suitable for
the shared official formula repository. A third-party tap remains the
maintainable upstream-controlled option.

## Conan

Build and test the checked-in recipe locally:

```sh
conan profile detect --force
conan create . --build=missing
```

The recipe does not enable `ENABLE_WASM_HANDOFF` by default. To distribute
the package through a private or organisation Conan remote, configure that
remote and credentials independently, then upload the tested package. Conan
Center Index has its own inclusion review and CLA process; a recipe in this
repository does not mean it has been accepted or published there.

## Debian and RPM

The project configures CPack to generate TGZ/ZIP and, on Linux, DEB/RPM
packages. Those files can be installed locally with the native package tools.
The manual DEB/RPM workflow can attach locally built packages to the GitHub Release after an explicit dispatch confirmation. It has not yet been run, and no hosted apt or dnf repository is advertised.

## Release checksum policy

The vcpkg and Homebrew recipes must use checksums calculated from the exact
immutable release source archive they advertise. The currently published
external recipes remain pinned to the verified 1.6.0 baseline. For v1.11.0,
wait until the immutable tag and source archive exist, calculate exact
SHA256/SHA512 values, then update the Homebrew formula and vcpkg port/version
database together in a separate package-registry change. Never use placeholders,
reuse a previous release checksum, or point an old checksum at a new tag.
