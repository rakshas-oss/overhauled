# Overhauled package distribution

Overhauled is a C++17 CMake library and optional broker service. Its primary
package-manager target is the CMake package, not a prebuilt platform-specific
binary.

## Recommended channels

| Channel | Intended use | Current state |
|---|---|---|
| vcpkg Git registry | C++ library and broker, CPU-only baseline | Registry recipe and 1.6.0 baseline remain in `main`. Automated vcpkg validation has been removed after repeated failures: the port still places CMake exports under `lib/cmake` instead of vcpkg's canonical `share/<port>` layout. The registry recipe is not currently CI-validated. |
| Homebrew formula | macOS/Linux developer installation | The 1.6.0 formula and immutable source checksum remain under `Formula/nvlink-placement.rb`. Automated Homebrew installation validation has been removed after repeatable macOS compilation failure in `src/broker_server.cpp` (`std::chrono` time-point/duration mismatch); the formula is not currently CI-validated. |
| Conan | Existing Conan consumers | Conan 2 recipe is in `main` and `conan create . --build=missing` passed; no remote upload is claimed. |
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
registry baseline to a commit that contains the port and versions database.

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

The formula targets the immutable 1.6.0 source tag with CUDA and TensorRT
disabled by default. Its automated macOS installation check is currently
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
immutable `1.6.0` source archive. Never reuse the old v1.1.1 formula hash or
the older moving `v1` tag hash. The CI workflow computes the two digests and
publishes them in the run summary before the recipes are updated.
