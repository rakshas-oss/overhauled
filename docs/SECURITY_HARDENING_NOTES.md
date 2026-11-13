# Security hardening notes

This note summarizes the defensive behavior applied to the placement and BRK2 response APIs.

- GPU placement rejects an empty topology before calculating a round-robin index, preventing modulo-by-zero on invalid configuration.
- Model registry insertions and lookups are protected by a mutex. Model loading is performed before taking the registry lock so expensive TensorRT compilation does not block readers; the lock protects map publication and lookup.
- BRK2 response task IDs remain subject to the configured string limit. Oversized error text is truncated to that limit rather than causing an otherwise valid response to fail serialization; payload limits remain enforced.

When enabling optional interop encryption, configure-time validation should fail clearly if the implementation source is missing. The TensorRT network listener is a binary protocol endpoint, not an authentication mechanism; keep it on a trusted network or behind an authenticated transport boundary.
