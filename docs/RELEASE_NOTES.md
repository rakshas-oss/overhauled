# Release Notes

Repository: rakshas-oss/overhauled

## v1.1.2
Date: 2026-11-13

### Highlights

- **BRK2 parser hardening**: Added O(1) TLV duplicate detection and enforced the shared 4,096-field limit.
- **Broker client tracking**: Added O(1) active-client ID collision checks alongside the bounded source LRU.
- **Placement concurrency**: Reduced affinity-lock scope so queue-depth callbacks run outside the affinity mutex.
- **Adversarial benchmark accuracy**: Correlated response latency by BRK2 request ID and removed synthetic fallback measurements.
- **Protocol boundary coverage**: Added conformance tests for the BRK2 field-count ceiling.

---

## v1.0.0 (Historical)
Date: 2026-10-11

This release formalizes the project as a stable, production-facing GPU placement and broker interoperability library. It consolidates the public contract, expands the documentation set, and makes the CPU-only and GPU-capable operational modes clearer for both development and production users.

Highlights

- Stable placement policy: NVLink-aware home-GPU affinity, peer-aware rebalancing, and deterministic fallback behavior.
- Broker interoperability: BRK1/v1 protocol is the supported external contract for request/response dispatch and service integration.
- CPU-only compatibility: the broker service can run without CUDA for smoke tests, CI, and non-GPU environments.
- Packaging readiness: Conan, vcpkg, and Homebrew guidance is aligned with the release version and publish flow.
- Documentation refresh: installation, API usage, deployment, operations, protocol, and benchmarking references are aligned to the v1.0 release.

Notable changes

- README.md refreshed as a v1.0 overview with quick-start guidance and release-oriented documentation links.
- docs/LIBRARY.md updated with the stable public API model and the current broker and placement flow.
- docs/PUBLISHING.md updated to use version 1.0.0 package metadata and release checklists.
- DEPLOYMENT_GUIDE.md updated to reference the v1.0 operational model and CPU/GPU build modes.
- The broker protocol and execution boundary are documented as the default supported integration contract for external systems.

Upgrade notes

- v1.0 follows the stable public API surface used by the placement library and broker service; it is the recommended baseline for new integrations.
- Users migrating from earlier v0.3 docs or examples should treat the BRK1/v1 contract as the canonical interface and use the current release notes and docs as the source of truth.
- For packaging, update any downstream version references to 1.0.0 before publishing or tagging a release build.

How to get the release

- From source:

```bash
git clone https://github.com/rakshas-oss/overhauled.git
cd overhauled
git fetch --tags origin
git checkout v1
```

- Package managers: see packaging/README.md and docs/PUBLISHING.md for current packaging guidance.

Acknowledgements

Thanks to all contributors, operators, and benchmark participants who helped stabilize the v1.0 release. If you have questions, open an issue or use the repository discussion and operations guides.
