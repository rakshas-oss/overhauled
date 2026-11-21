# NVLink Placement - Publishing Guide

Complete guide for publishing `nvlink_placement` to major C++ package managers.

## Quick Summary

This library is ready to be published to:

✅ **Conan** - Primary recommendation
✅ **vcpkg** - Microsoft's package manager
✅ **Homebrew** - macOS/Linux
⏳ **apt/dnf** - Linux distributions

---

## 1. Conan (Recommended)

### What's Included
- ✅ `conanfile.py` - Complete Conan recipe
- ✅ CMakeDeps integration
- ✅ CUDA dependency handling

### Publish to Conan Center Index

```bash
# 1. Fork conan-center-index
git clone https://github.com/conan-io/conan-center-index.git
cd conan-center-index
git checkout -b nvlink_placement

# 2. Create package directory
mkdir -p recipes/nvlink_placement/all
cp /path/to/nvlink_placement/conanfile.py recipes/nvlink_placement/all/

# 3. Create config file
cat > recipes/nvlink_placement/config.yml << 'EOF'
versions:
  "1.5.0":
    folder: all
EOF

# 4. Test locally
conan create . --build=missing

# 5. Push and create PR
git add recipes/nvlink_placement/
git commit -m "Add nvlink_placement/1.5.0"
git push origin nvlink_placement
# Create PR on GitHub
```

### Usage After Publishing

```cpp
// conanfile.txt
[requires]
nvlink_placement/1.5.0

[generators]
CMakeDeps
CMakeToolchain
```

```bash
conan install . --build=missing
cmake --preset conan-default
cmake --build --preset conan-release
```

---

## 2. vcpkg (Microsoft Package Manager)

### What's Included
- ✅ `vcpkg.json` - Manifest format
- ✅ `vcpkg-portfile.cmake` - CMake-based port

### Publish to vcpkg

```bash
# 1. Fork vcpkg repository
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
git checkout -b nvlink-placement

# 2. Create port directory
mkdir -p ports/nvlink-placement

# 3. Add port files
cp /path/to/nvlink_placement/vcpkg.json ports/nvlink-placement/
cp /path/to/nvlink_placement/vcpkg-portfile.cmake ports/nvlink-placement/portfile.cmake

# 4. Add version file
cat > ports/nvlink-placement/vcpkg.json << 'EOF'
{
  "name": "nvlink-placement",
  "version": "1.5.0"
}
EOF

# 5. Test installation
./vcpkg install nvlink-placement:x64-linux --build=missing

# 6. Push and create PR
git add ports/nvlink-placement/
git commit -m "Add nvlink-placement port"
git push origin nvlink-placement
# Create PR on GitHub
```

### Usage After Publishing

```bash
# Install
vcpkg install nvlink-placement

# Use in CMake
find_package(nvlink_placement REQUIRED)
target_link_libraries(myapp nvlink_placement)
```

---

## 3. Homebrew (macOS/Linux)

### What's Included
- ✅ `nvlink-placement.rb` - Homebrew formula

### Publish to Homebrew

```bash
# 1. Fork homebrew-core
git clone https://github.com/Homebrew/homebrew-core.git
cd homebrew-core
git checkout -b nvlink-placement

# 2. Copy formula
cp /path/to/nvlink_placement/nvlink-placement.rb Formula/

# 3. Test formula locally
brew install --build-from-source ./Formula/nvlink-placement.rb
brew test nvlink-placement

# 4. Lint formula
brew audit --strict Formula/nvlink-placement.rb

# 5. Push and create PR
git add Formula/nvlink-placement.rb
git commit -m "Add nvlink-placement formula"
git push origin nvlink-placement
# Create PR on GitHub
```

### Usage After Publishing

```bash
brew install nvlink-placement

# Find installation location
brew --prefix nvlink-placement
```

---

## 4. Linux Distribution Packages

### Ubuntu/Debian (apt)

Create PPA:

```bash
# Setup launchpad account and GPG key
# Create debian/ package files
mkdir -p debian

# Upload to PPA
dput ppa:your-username/nvlink-placement nvlink-placement_1.5.0_source.changes
```

Usage:
```bash
sudo add-apt-repository ppa:your-username/nvlink-placement
sudo apt-get update
sudo apt-get install libnvlink-placement-dev
```

### Fedora/RHEL (dnf/yum)

Create RPM:

```bash
# Build RPM
rpmbuild -ba nvlink-placement.spec

# Upload to Fedora Copr
copr-cli build @nvidia/cuda nvlink-placement.spec
```

Usage:
```bash
sudo dnf install nvlink-placement-devel
```

---

## Published v1.5.0 and v1.5.1 patch candidate

Overhauled v1.5.0 is published. Its optional native Wasmtime composition feature was released with a Wasmtime v51 C API integration defect. The fix is merged on main and the v1.5.1 patch candidate metadata is in place. The corrected main merge commit passed all 8 reported CI checks on 2026-11-21, including the Wasmtime v51 runtime lane, BRK2 mutation tests, paired YuKKi-OS interoperability, sanitizers and Ubuntu/Windows installed-package consumer checks. v1.5.1 is still not a published release: verify the exact release/tag commit and GitHub release assets before describing it as released.

For the v1.5.1 candidate:

- CMake, Conan and the root vcpkg manifest are aligned to 1.5.1 on the candidate branch.
- The Wasmtime C API CI lane pins v51.0.0 on Linux x86_64. The candidate must pass the Wasmtime-enabled compile **and** provider/consumer runtime test.
- The default build, BRK2 conformance, sanitizer, package-consumer and paired YuKKi-OS interoperability gates must also pass on the exact release commit.
- Do not update Homebrew, vcpkg portfile archive hashes, or other external package registries until the final v1.5.1 archive exists. Generate checksums from that exact archive; never reuse a checksum from a different tag.
- A GitHub release does not automatically publish Conan Center, vcpkg, Homebrew, apt or dnf packages. Submit and verify each registry independently.

## Release checklist

Before publishing a patch release:

- [ ] Confirm the intended source tag and published baseline.
- [ ] Update CMakeLists.txt, conanfile.py and vcpkg.json.
- [ ] Update README, versioning and release notes with accurate candidate status.
- [ ] Pass default native CI and the pinned Wasmtime C API compile/runtime lane.
- [ ] Pass sanitizer, BRK2 conformance/mutation, package-consumer and paired YuKKi-OS interoperability checks on the exact release commit.
- [ ] Review and merge the release PR.
- [ ] Create and push the version tag from the exact approved merge commit.
- [ ] Verify the tag target, GitHub release and source archive.
- [ ] Generate vcpkg SHA512 and Homebrew SHA256 from that exact archive.
- [ ] Submit and independently verify any desired external package-manager registry updates.

## Verification

After publishing to each package manager:

### Conan
```bash
conan search nvlink_placement/1.5.0@
conan install --requires=nvlink_placement/1.5.0
```

### vcpkg
```bash
vcpkg search nvlink-placement
vcpkg install nvlink-placement
```

### Homebrew
```bash
brew search nvlink-placement
brew install nvlink-placement
```

---

## Documentation Links

### Publishing Guides
- [Conan Publishing](https://docs.conan.io/en/latest/)
- [vcpkg Creating Ports](https://github.com/Microsoft/vcpkg/tree/master/docs/maintainers)
- [Homebrew Formula Cookbook](https://docs.brew.sh/Formula-Cookbook)

### Repository Links (After Publishing)
- Conan Center Index: https://conanio.org/center/nvlink_placement
- vcpkg: https://github.com/Microsoft/vcpkg/tree/master/ports/nvlink-placement
- Homebrew: https://formulae.brew.sh/formula/nvlink-placement

---

## Maintenance

Once published:

1. **Monitor issues** - Users may report build issues
2. **Update recipes** - Keep versions in sync
3. **Security updates** - Act quickly on CVEs
4. **Version bumps** - Release new versions regularly
5. **Documentation** - Keep usage guides current

---

## Support

For questions about publishing:
- Conan Community: https://slack.conan.io
- vcpkg Issues: https://github.com/Microsoft/vcpkg/issues
- Homebrew Discussions: https://github.com/Homebrew/homebrew-core/discussions
