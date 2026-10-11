# Provenance review

## Scope and method

This review covered the tracked C++ source and headers, examples, tests,
benchmark script, Python scripts, build/package configuration, and existing
license and documentation files. The files examined were:

- `src/adi_server.cpp`, `src/broker_protocol.cpp`, `src/broker_server.cpp`,
  `src/broker_server_main.cpp`, `src/broker_service.cpp`,
  `src/geospatial_frame.cpp`, `src/gpu_runtime_server.cpp`,
  `src/media_stream.cpp`, `src/nvlink_placement.cpp`, and
  `src/wasm_sandbox.cpp`.
- `include/adi_server.h`, `include/adi_tensor_protocol.h`,
  `include/backend_dispatch.h`, `include/broker_protocol.h`,
  `include/broker_server.h`, `include/broker_service.h`,
  `include/geospatial_frame.h`, `include/media_stream.h`,
  `include/nvlink_placement.h`, `include/trt_model_engine.hpp`, and
  `include/wasm_sandbox.h`.
- `examples/adi_server_example.cpp`, `examples/minimal.cpp`,
  `examples/multi_client.cpp`, `examples/test_tensor_client.py`,
  `tests/backend_dispatch_test.cpp`,
  `tests/broker_async_latency_benchmark_test.cpp`,
  `tests/broker_protocol_test.cpp`, `tests/broker_server_e2e_test.cpp`,
  `tests/broker_service_test.cpp`, `tests/geospatial_frame_test.cpp`,
  `tests/media_stream_test.cpp`, `tests/wasm_sandbox_test.cpp`,
  `tests/benchmark_comprehensive.py`, `tests/heuristic_analyzer.py`, and
  `benchmarks/run_benchmarks.sh`.
- `CMakeLists.txt`, `conanfile.py`, `vcpkg.json`, `vcpkg-portfile.cmake`,
  `nvlink-placement.rb`, `LICENSE`, README files, deployment and publishing
  guides, and other tracked documentation.

The review enumerated tracked files, inspected source-file preambles and
imports, and searched the source tree for copyright/license notices, SPDX
identifiers, generated/vendor markers, and common placeholder/attribution
signals. The implementation and repeated patterns were also manually sampled
for abrupt style changes or recognizable copied blocks. No dedicated
code-origin matching database or license scanner was run.

## Findings

- `LICENSE` contains the repository's MIT license and identifies
  `NVLink Placement Contributors` with copyright year 2024.
- No pre-existing third-party copyright/license headers, vendor/third-party
  source directories, or generated-code markers were found in the source,
  headers, examples, tests, or benchmark script examined above.
- No likely verbatim third-party source block or clear abrupt coding-style
  outlier was identified in the manual review. This is a limited observation,
  not proof of authorship or originality.
- CUDA Toolkit/cuBLAS and TensorRT headers and libraries are external optional
  NVIDIA dependencies; they are not source files vendored in this repository.
- The repository had tracked `build-debug/` CMake test metadata, test logs,
  and compiled executables. Those generated build outputs have been removed
  from version control and `build*/` is ignored.
- Python import review found NumPy, PyTorch, ONNX, and Conan as external
  packages. Standard-library imports do not need to be listed in
  `requirements.txt`; package license references are recorded in `NOTICE`.

## Files with no provenance flags found

The files listed under **Scope and method** had no third-party provenance
flags in this review. The NVIDIA dependency references and formerly tracked
build outputs described above are the only noteworthy external/generated
material found; neither constitutes vendored first-party source.

## Limitations and recommended follow-up

An automated text/pattern scan and manual inspection can only flag likely
matches. They cannot prove that code is original, establish complete
authorship, or replace legal review. Before a formal compliance attestation,
run a dedicated scanner such as ScanCode Toolkit and review its output:

```bash
scancode --license --copyright --package --info --strip-root \
  --json-pp scancode-results.json .
```

Review flagged matches and package metadata manually; consider FOSSA or Black
Duck as an additional source/dependency review.
