# BRK1 / BRK2 Executable Conformance v1

This document defines the shared executable byte corpus consumed independently by the Rust YuKKi-OS codec and the C++ overhauled broker codec.

## Normative corpus

The normative vectors are in `tests/fixtures/protocol/brk-conformance-v1.corpus` in both repositories. The two files must remain byte-for-byte identical. Each non-comment line has five pipe-separated columns:

```text
case_id|protocol|direction|expectation|body_hex
```

- `body_hex` is the protocol body only; it excludes the four-byte TCP big-endian length prefix for both BRK1 and BRK2.
- `expectation` is `accept` or `reject`, at the request/response codec and schema validation boundary implemented by the runner.
- Hexadecimal is lowercase and has no separators.
- Case IDs are stable. A behavior change requires an explicit corpus/spec review, not silently updating expected outcomes.

## Execution

Rust:

```sh
cargo test --locked --test brk_conformance
```

C++:

```sh
cmake -S . -B build -DENABLE_CUDA=OFF -DENABLE_TENSORRT=OFF -DBUILD_BROKER=ON -DBUILD_TESTING=ON
cmake --build build --target brk_conformance_test --parallel 2
ctest --test-dir build --output-on-failure -R '^brk_conformance_test$'
```

The cross-repository YuKKi-OS integration job checks the mirrored corpus files with `cmp`, then executes both runners against the same checked-out `main` revisions and records both commit SHAs.

## Coverage in v1

- BRK1 request decoding and canonical request encoding.
- BRK1 response decoding, including status validation.
- BRK2 request decoding, schema validation, request-ID preservation, and unknown optional/critical extension handling.
- Truncation, trailing bytes, invalid magic, reserved fields, zero timeout, invalid response status, and invalid fixed-width TLV values.

The corpus does not claim to test WSM1 payload semantics, TCP stream deadlines, concurrent request correlation, or coverage-guided fuzzing. Those remain separate test layers. No production wire format is changed by this test milestone.
