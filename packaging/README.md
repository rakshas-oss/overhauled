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

| Manager | Status | Version |
|---------|--------|----------|
| Conan | Ready | 1.0.0 |
| vcpkg | Ready | 1.0.0 |
| Homebrew | Ready | 1.0.0 |
| apt (Ubuntu) | In Progress | 1.0.0 |
| dnf (Fedora) | In Progress | 1.0.0 |
