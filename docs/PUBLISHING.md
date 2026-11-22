# Overhauled package publishing

This guide distinguishes a tested package recipe from an actual registry
publication. The v1.6.0 GitHub Release is published; external package-manager
registries are independent distributions.

## Current distribution status

| Channel | Status | Intended package |
|---|---|---|
| vcpkg Git registry | v1.6.0 port and version baseline prepared in this repository; overlay build validation pending | `nvlink-placement` |
| Homebrew | v1.6.0 CPU-only formula prepared; no dedicated third-party tap has been created | `nvlink-placement` |
| Conan | Conan recipe is checked in and versioned 1.6.0; no remote upload is claimed | `nvlink_placement/1.6.0` |
| Conan Center Index | Not submitted; inclusion review and CLA are required | `nvlink_placement` |
| Debian/RPM | CPack supports local DEB/RPM generation on Linux; no hosted apt/dnf repository is configured | `OverhauledTensorRuntime` |

The v1.6.0 source archive is immutable and pinned in the vcpkg and Homebrew
recipes:

- SHA512: `bc809690c6cb092da7c84f53a06a40899a227b5ad1ffb08b6d4dac3965a6e12219509b05badd2e3cc44e3d006c41464a0e9044f49bcbc7dc33628dcd22437311`
- SHA256: `399d29709086482db582de3ad4ecb1f1312925f415b238a036878e1f9863881d`

Source: `https://github.com/rakshas-oss/overhauled/archive/refs/tags/1.6.0.tar.gz`.

## vcpkg Git registry

This repository contains a Git-based vcpkg registry:
- `ports/nvlink-placement/` contains the 1.6.0 port.
- `versions/n-/nvlink-placement.json` records the exact port tree.
- `versions/baseline.json` selects 1.6.0 as the default baseline.

For a quick local overlay install before configuring a registry:

```sh
vcpkg install nvlink-placement --overlay-ports=/path/to/overhauled/ports/nvlink-placement
```

For a consumer project, add this additional registry to its
`vcpkg-configuration.json`. Replace `REGISTRY_BASELINE_COMMIT` with the
merge commit that introduces the registry files:

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

Then run:

```sh
vcpkg install nvlink-placement
```

The registry port deliberately builds the CPU-only portable configuration.
It does not enable CUDA, TensorRT, Wasmtime, or the optional OpenSSL WASM
handoff receiver. Windows triplets are excluded because mainline Windows/MSVC
is not a supported build target.

## Homebrew

The formula uses the immutable 1.6.0 source archive, builds shared libraries,
and disables optional CUDA/TensorRT/Wasmtime/handoff dependencies. There is no
official Homebrew Core acceptance or maintained third-party tap yet.

Install the checked-in formula directly:

```sh
brew install https://raw.githubusercontent.com/rakshas-oss/overhauled/main/nvlink-placement.rb
```

For discoverable tap-based installation, create and maintain a separate
`homebrew-<tap>` repository and copy the formula into its root. Homebrew Core
submission is a separate review and must pass the official build/test matrix.

## Conan

The checked-in recipe builds the CPU-only default without enabling the optional
WASM handoff receiver. Test locally:

```sh
conan profile detect --force
conan create . --build=missing
```

To upload to an organisation's Conan remote, first configure credentials and
the remote in the user's Conan client, then:

```sh
conan upload nvlink_placement/1.6.0 --remote=<remote-name> --confirm
```

This repository does not contain remote credentials. Uploading to a private
Conan remote is separate from submitting a recipe to Conan Center Index. CCI
requires review, including its inclusion criteria and contributor agreement;
a checked-in recipe does not mean acceptance or publication.

## Debian and RPM

The CMake project enables CPack DEB and RPM generators on Linux. Build local
packages with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BROKER=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
  -DENABLE_CUDA=OFF -DENABLE_TENSORRT=OFF \
  -DENABLE_WASM_RUNTIME=OFF -DENABLE_WASM_HANDOFF=OFF
cmake --build build --parallel
cpack --config build/CPackConfig.cmake -G DEB -B packages
cpack --config build/CPackConfig.cmake -G RPM -B packages
```

These packages can be installed locally with `apt install ./package.deb` or
`dnf install ./package.rpm`. This is not the same as an indexed apt/dnf
repository; signed repository metadata and hosting still need to be configured.

## Security and release boundaries

- The 1.6.0 release is a GitHub source release. Package recipes must continue
  to use its immutable tag and verified checksum.
- Default Conan/vcpkg/Homebrew packages do not enable
  `ENABLE_WASM_HANDOFF`. That feature requires OpenSSL Crypto and separate
  feature-specific CI validation.
- The handoff implementation remains receiver primitives, not a network-facing
  authenticated BRK2 service.
- No package registry credentials belong in source control. Use each registry's
  supported secret, token or trusted-publishing mechanism.
- Do not advertise a package as published until it appears in the registry and
  a clean consumer installation has succeeded.
