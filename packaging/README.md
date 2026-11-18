# Package Manager Configuration Files

This directory contains configuration for publishing `nvlink_placement` to package managers.

## Files Included

### Conan
- `conanfile.py` - Complete Conan recipe
  - Handles CUDA dependency
  - CMake integration
  - Build and install rules

### vcpkg
- `vcpkg.json` - Package manifest
- `vcpkg-portfile.cmake` - CMake build rules
  - GitHub source integration
  - Header and library installation

### Homebrew
- `nvlink-placement.rb` - Homebrew formula
  - macOS and Linux support
  - CUDA toolkit dependency
  - Test suite

## Publishing Steps

See [PUBLISHING.md](../docs/PUBLISHING.md) for complete instructions.

## Quick Commands

### Conan
```bash
conan create . --build=missing
```

### vcpkg
```bash
vcpkg install nvlink-placement
```

### Homebrew
```bash
brew install nvlink-placement
```

## Status

| Manager | Repository state | Version / next step |
|---------|-------------------|----------------------|
| Conan | Recipe aligned; registry submission not verified | 1.1.3 |
| vcpkg | Manifest aligned; registry submission not verified | 1.1.3 |
| Homebrew | Formula checksum refresh required after tag creation | Pending |
| apt (Ubuntu) | Not published | Pending |
| dnf (Fedora) | Not published | Pending |