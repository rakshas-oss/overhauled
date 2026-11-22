# Package-manager configuration

This repository's package identity is `nvlink-placement` / Conan
`nvlink_placement`, version 1.6.0.

## Maintained package definitions

- `ports/nvlink-placement/` — vcpkg Git-registry port and manifest.
- `versions/baseline.json` and `versions/n-/nvlink-placement.json` — vcpkg
  registry baseline and immutable port-tree record.
- `nvlink-placement.rb` — Homebrew formula pinned to the 1.6.0 source tag.
- `conanfile.py` — Conan recipe at 1.6.0.
- `vcpkg-portfile.cmake` — compatibility overlay for users of the legacy
  root-level portfile.

The vcpkg port and Homebrew formula use the verified 1.6.0 source archive
checksums. The vcpkg port is CPU-only and excludes Windows/MSVC triplets.
Homebrew builds the shared library and broker without requiring CUDA.

## Validation

- The vcpkg workflow bootstraps vcpkg, builds the overlay port from the pinned
  source archive and compiles/runs an installed CMake package consumer.
- The standard CI checks continue to validate the native broker, BRK2,
  sanitizers, Wasmtime and paired YuKKi-OS interoperability.
- The optional `ENABLE_WASM_HANDOFF=ON` receiver lane is independent of the
  portable package defaults.

See [the publishing guide](../docs/PUBLISHING.md) and
[package overview](../docs/PACKAGING.md) for registry configuration, consumer
commands and the boundary between recipe readiness and actual publication.

## Registry status

| Manager | Current state |
|---|---|
| vcpkg Git registry | Repository-hosted 1.6.0 baseline and port prepared; consumer validation pending |
| Homebrew | 1.6.0 source formula; no official Core acceptance or dedicated tap |
| Conan | Recipe ready for local build; no remote upload performed |
| Conan Center Index | Not submitted; contributor agreement and inclusion review apply |
| apt/dnf | CPack supports local DEB/RPM files; no hosted signed repository |
