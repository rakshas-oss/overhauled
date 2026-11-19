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
| Conan | Recipe aligned to 1.1.5 candidate; registry submission not verified | 1.1.5 |
| vcpkg | Manifest aligned to 1.1.5 candidate; portfile source hash pending final archive | 1.1.5 |
| Homebrew | Checked-in formula remains v1.1.1; URL/SHA256 refresh required from final 1.1.5 archive | Pending |
| apt (Ubuntu) | Not published; separate submission required | Pending |
| dnf (Fedora) | Not published; separate submission required | Pending |