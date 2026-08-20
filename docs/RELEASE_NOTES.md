# Release Notes

Repository: rakshas-oss/overhauled

Version: v0.3
Date: 2026-08-20

This release packages improvements to topology detection, placement policy, performance, packaging, and documentation.

Highlights

- NVLink-aware placement: improved rebalancing to prefer NVLink peers for reduced host-staging and lower tail latency.
- Performance: validated improvements from benchmarks — ~27% lower mean latency and up to 98% better p99 tail latency versus blind round-robin on multi-client workloads (see docs/BENCHMARK_RESULTS.md).
- API stability: Placer and GpuTopology APIs stabilized; added clearer documentation for GpuTopology::enable_peer_access and Placer configuration (backlog_threshold).
- Thread-safety: confirmed immutable GpuTopology and concurrent-safe placement decisions; reduced locking in Placer where possible.
- Packaging: updated package manifests for Conan, vcpkg and Homebrew; portfile and formula included in packaging/.
- Documentation: consolidated docs, added this release notes file, fixed README links and updated docs index.

Notable changes

- New/changed files
  - docs/RELEASE_NOTES.md (this file)
  - README.md (links to release notes, fixed doc filenames)

- API
  - Placer::place(...) behaviour documented more clearly when home GPU is at threshold.
  - GpuTopology::enable_peer_access() noted as required once after detect() to enable P2P where applicable.

Bug fixes

- Fix: Avoid fall-through to host-staging when an NVLink peer is available but was previously missed due to tie-breaking logic.
- Fix: Corrected README doc links and removed references to non-existent docs (BENCHMARKING.md / NVLINK_PLACEMENT.md).

Upgrade notes

- If you previously relied on internal topology representations, switch to the public GpuTopology API — internal layout may change in minor releases.
- Re-run packaging/publish steps after bumping version strings in conanfile.py and vcpkg.json when publishing.

How to get the release

- From source:

```bash
git clone https://github.com/rakshas-oss/overhauled.git
cd overhauled
# checkout tag v0.3 when available
```

- Package managers: see packaging/README.md and docs/PUBLISHING.md for current packaging status (Conan, vcpkg, Homebrew supported).

Acknowledgements

Thanks to all contributors and benchmark participants. If you have questions about the release, open an issue or join the discussion in the repo.

