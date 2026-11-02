# Ethos-U85 model bundle (YU85BND1)

The mainline Linux DRM executor consumes a Vela-compiled command stream and
its preallocated memory regions. It does **not** translate arbitrary doubles
into an NPU graph. The broker's existing vector interface is an adapter for a
single signed-int8 input tensor and a single signed-int8 output tensor.

All integers and IEEE-754 float32 values are little-endian. The file layout is:

| Field | Type | Meaning |
|---|---:|---|
| Magic | 8 bytes | ASCII `YU85BND1` |
| Version | u32 | 1 |
| SRAM size | u32 | bytes requested for the job |
| Command-stream size | u32 | Vela command-stream bytes |
| Region count | u32 | 1..8 |
| Input region | u32 | region index containing input tensor |
| Output region | u32 | region index containing output tensor |
| Input offset | u32 | byte offset in input region |
| Output offset | u32 | byte offset in output region |
| Input elements | u32 | signed-int8 input count |
| Output elements | u32 | signed-int8 output count |
| Input scale | f32 | positive quantization scale |
| Input zero point | i32 | signed-int8 zero point |
| Output scale | f32 | positive dequantization scale |
| Output zero point | i32 | signed-int8 zero point |
| Command stream | bytes | Vela-generated U85 command stream |
| Regions | repeated | u32 byte length followed by region bytes |

The input and output tensor ranges must fit inside their selected regions.
The bundle must contain the Vela memory-region layout expected by its command
stream. The runtime copies each region into a DRM buffer object, patches the
input tensor, submits one job with the command BO and up to eight region BOs,
waits on the output BO, and dequantizes the output.

The executor uses the mainline `DRM_IOCTL_ETHOSU_*` ABI in
`include/uapi/drm/ethosu_accel.h`. It discovers writable render nodes
`/dev/dri/renderD128..` and can be pinned with `ETHOSU_DRM_DEVICE` or the
`--ethosu-device` option. It is opt-in: use `--npu-ethosu-bundle path`.
The simulated executor remains available only through `--npu-simulated`.

This implementation requires a kernel exposing the mainline Ethos-U DRM UAPI
and a command stream compiled for the attached U85. The broker CI can compile
and test the bundle parser without physical hardware; successful compilation
is not evidence of a board-level inference run.
