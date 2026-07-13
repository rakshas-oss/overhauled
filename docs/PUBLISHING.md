# NVLink Placement - Publishing Guide

Complete guide for publishing `nvlink_placement` to major C++ package managers.

## Quick Summary

This repository now includes package metadata for:

✅ **Conan** - Primary recommendation
✅ **vcpkg** - Port files for upstream submission
✅ **Homebrew** - Formula for a tap or upstream submission
⏳ **apt/dnf** - Linux distributions

---

## 1. Conan (Recommended)

### What's Included
- ✅ `conanfile.py` - Complete Conan recipe
- ✅ CMakeDeps integration
- ✅ CUDA dependency handling
- ✅ Installed CMake package config for consumers

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
  "1.0.0":
    folder: all
EOF

# 4. Test locally
conan create . --build=missing

# 5. Push and create PR
git add recipes/nvlink_placement/
git commit -m "Add nvlink_placement/1.0.0"
git push origin nvlink_placement
# Create PR on GitHub
```

### Usage After Publishing

```bash
conan install . --build=missing
cmake --preset conan-default
cmake --build --preset conan-release
```

```cmake
find_package(nvlink_placement CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE nvlink_placement::nvlink_placement)
```

---

## 2. vcpkg (Microsoft Package Manager)

### What's Included
- ✅ `vcpkg.json` - Manifest format
- ✅ `vcpkg-portfile.cmake` - CMake-based port
- ✅ Exported CMake package config for `find_package`

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
  "version": "1.0.0"
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
find_package(nvlink_placement CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE nvlink_placement::nvlink_placement)
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

# Use the installed CMake package
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix nvlink-placement)"
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
dput ppa:your-username/nvlink-placement nvlink-placement_1.0.0_source.changes
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

## Release Checklist

Before each release:

- [ ] Update version in `CMakeLists.txt`
- [ ] Update version in `vcpkg.json`
- [ ] Update version in `nvlink-placement.rb`
- [ ] Refresh release archive hashes in `vcpkg-portfile.cmake` and `nvlink-placement.rb`
- [ ] Update `README.md` with latest features
- [ ] Create or update the source release tag used by package managers
- [ ] GitHub release automatically created
- [ ] Submit PRs to Conan Center Index
- [ ] Submit PRs to vcpkg
- [ ] Submit PRs to Homebrew (optional)
- [ ] Announce on social media/forums

---

## Verification

After publishing to each package manager:

### Conan
```bash
conan search nvlink_placement/1.0.0@
conan install --requires=nvlink_placement/1.0.0
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
