# YuKKi-OS interoperability

This guide records the Overhauled-facing contract for YuKKi-OS and other control-plane clients. YuKKi-OS remains the control plane; Overhauled owns placement and execution. There is no direct source dependency between the projects.

## Transport and protocol selection

The broker accepts TCP frames with a 4-byte unsigned big-endian body-length prefix. The frame body is selected by its magic: BRK1/v1 or BRK2/v2. Both may be used on the same listener. The client must send the desired binary format; the server does not negotiate it through JSON HELLO messages.

YuKKi-OS's generic JSON broker client is a different protocol and must not be pointed at the Overhauled C++ broker. Use the YuKKi GPU adapter's BRK1/BRK2 client and WSM1 task/lifecycle payloads for this service.

- [BRK1/BRK2 wire protocol](BROKER_PROTOCOL.md)
- [BRK2 cross-language conformance](BRK2_CONFORMANCE_V1.md)
- [Executable BRK1/BRK2 conformance corpus](BRK_CONFORMANCE_V1.md)
- [WASM task and lifecycle payloads](WASM_INTEROP.md)

## BRK2 backend selection

BRK2 request field 10, `device_preference`, is optional and accepts `cpu`, `gpu`, or `npu`; unknown values are rejected. The broker uses the preference during backend dispatch. BRK2 response field 7, `backend`, reports the selected/executing backend (`cpu`, `gpu`, or `npu`) so a client can verify the outcome instead of interpreting its request as proof of hardware execution.

A request that asks for NPU does not guarantee that an NPU executor is available or selected. CPU fallback remains governed by the request's fallback-safety policy. `--npu-simulated` installs a CPU-emulated executor for CI and conformance testing; it is not a hardware acceleration path.

## Ethos-U85 physical execution

The real Ethos-U85 execution path is an opt-in Linux mainline DRM backend. It uses the upstream `DRM_IOCTL_ETHOSU_*` UAPI and a Vela-generated `YU85BND1` bundle describing the command stream, memory regions, signed-int8 tensors, and quantization parameters.

```sh
cmake -S . -B build \
  -DENABLE_ETHOSU_DRM=ON \
  -DENABLE_CUDA=OFF \
  -DENABLE_TENSORRT=OFF \
  -DBUILD_BROKER=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/broker_server 9090 --npu-ethosu-bundle model.yu85
```

Optionally pin the DRM node with `--ethosu-device /dev/dri/renderD128`. The bundle format and runtime requirements are in [ETHOSU_U85_BUNDLE.md](ETHOSU_U85_BUNDLE.md).

Compilation and CPU simulator tests do not prove physical execution. Hardware qualification requires a kernel exposing the mainline Ethos-U DRM UAPI, a bundle compatible with the target board, correct input/output quantization, and a successful inference on that board. YuKKi-OS's `src/drivers/ethosu85_async.rs` is an orphaned placeholder, not a DRM driver.

## Validation and security boundary

Both language implementations run the shared BRK conformance corpus; YuKKi-OS's live interoperability workflow exercises a real CPU-only C++ broker over TCP. Such tests establish protocol and software behavior, not physical NPU qualification.

The broker protocol is not an authentication or encryption layer. Use a trusted network or independently secured proxy. The protocol does not provide JSON handshake negotiation or remote cancellation; closing a client socket does not guarantee preemption of work already running. Explicit task timeouts are limited to `1..=300000` ms. Configure client frame and payload limits no higher than the broker's `ProtocolLimits`.
