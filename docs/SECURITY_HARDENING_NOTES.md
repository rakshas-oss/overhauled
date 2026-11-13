# Security hardening notes

This note summarizes defensive checks applied to Overhauled's placement, broker framing, and optional TensorRT runtime.

- GPU placement rejects an empty topology before round-robin selection.
- The TensorRT model registry synchronizes model publication and lookup. Loading/compilation occurs outside the registry lock.
- BRK2 task IDs remain subject to the configured string limit; oversized error messages are truncated to fit. Payload size limits are unchanged.
- Enabling interop encryption fails clearly at configure time if its implementation source is absent.
- TensorRT runtime input is bounded, read and written in exact byte loops, and validated before dispatch. ONNX paths are constrained by the runtime's accepted path policy.

The TCP/TensorRT endpoint is a binary protocol endpoint, not an authentication layer. Expose it only on a trusted network or behind an authenticated transport boundary.
