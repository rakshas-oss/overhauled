# Overhauled package distribution

Overhauled is a C++17 CMake library and optional broker service. Its primary
package-manager target is the CMake package, not a prebuilt platform-specific
binary.

## Recommended channels

| Channel | Intended use | Current state |
|---|---|---|
| vcpkg Git registry | C++ library and broker, CPU-only baseline | Registry recipe being added to this repository |
| Homebrew formula | macOS/Linux developer installation | Formula being refreshed for 1.6.0; start with direct formula installation or maintain a third-party tap |
| Conan | Existing Conan consumers | Recipe remains available; package build and remote upload are separate |
| Conan Center Index | Public curated Conan package | Not submitted; CCI reviews project maturity, usage and maintenance. An upstream PR is a separate contribution |
| Debian/RPM | System package installation | CPack emits native packages when built on Linux; no hosted apt/dnf repository is currently published |

## vcpkg Git registry

The repository-hosted Git registry provides a versioned port for `nvlink-placement`.
Consumers configure the repository as an additional Git registry and pin the
registry baseline to a commit that contains the port and versions database.

A consumer's `vcpkg-configuration.json` should include an entry like this,
replacing `REGISTRY_BASELINE_COMMIT` with the merge commit that adds the registry:

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
      "baseline": "REGISTRY_BASELINE_COMMIT",
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
and are not silently enabled by the portable registry build.

## Homebrew

The formula builds from the immutable 1.6.0 source tag with CUDA and TensorRT
disabled by default. Until a dedicated tap repository is created, consumers
can install the maintained formula directly from its raw GitHub URL:

```sh
brew install https://raw.githubusercontent.com/rakshas-oss/overhauled/main/nvlink-placement.rb
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
A hosted apt or dnf repository requires additional signed repository metadata
and hosting; no such repository is currently advertised.

## Release checksum policy

The vcpkg and Homebrew recipes must use checksums calculated from the exact
immutable `1.6.0` source archive. Never reuse the old v1.1.1 formula hash or
the older moving `v1` tag hash. The CI workflow computes the two digests and
publishes them in the run summary before the recipes are updated.
