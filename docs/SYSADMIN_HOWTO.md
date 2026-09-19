# Sysadmin How-To: broker_server with YuKKi-OS

This guide is for Linux administrators deploying `broker_server` from this repository as the overhauled execution plane behind an external authenticated control plane such as `rakshas-oss/YuKKi-OS`.

## 1. What this component is

- `overhauled`: GPU topology, placement, and execution plane
- `YuKKi-OS`: external authenticated control plane
- interoperability boundary: raw TCP carrying the versioned `BRK1` / protocol version `1` broker frames documented in [BROKER_PROTOCOL.md](BROKER_PROTOCOL.md)

This repository does **not** embed YuKKi-OS and does **not** provide built-in TLS, authentication, or an HTTP health API for the broker.

## 2. Prerequisites

Required:

- Linux host
- CMake 3.18+
- C++17 compiler
- POSIX sockets / pthread-capable runtime

Optional for GPU-enabled placement:

- NVIDIA GPU(s)
- CUDA toolkit/runtime detectable by CMake (`-DENABLE_CUDA=ON`)

Optional for packaging / installation:

- root or package-build access for `make install`

## 3. Build and install

Repository-root build, CPU-only:

```bash
cd /home/runner/work/overhauled/overhauled
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=OFF \
  -DENABLE_TENSORRT=OFF
cmake --build build -j"$(nproc)"
```

Repository-root build, GPU-enabled when CUDA is available:

```bash
cd /home/runner/work/overhauled/overhauled
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=ON \
  -DENABLE_TENSORRT=OFF
cmake --build build -j"$(nproc)"
```

Optional install into the default prefix:

```bash
cd /home/runner/work/overhauled/overhauled/build
sudo cmake --install .
```

Installed artifacts of interest:

- broker binary: `bin/broker_server`
- libraries/headers: installed by CMake for embedding or packaging

## 4. Suggested service account and filesystem layout

Illustrative example values:

- service account: `overhauled`
- install root: `/opt/overhauled`
- runtime working directory: `/var/lib/overhauled`
- log capture directory (if your supervisor redirects stdout/stderr): `/var/log/overhauled`

Minimal setup example:

```bash
sudo useradd --system --home /var/lib/overhauled --shell /usr/sbin/nologin overhauled
sudo install -d -o overhauled -g overhauled /var/lib/overhauled
sudo install -d -o overhauled -g overhauled /var/log/overhauled
```

The broker itself does not require a writable config file or database. Keep filesystem permissions minimal and treat logs as stdout/stderr capture owned by your local process supervisor policy.

## 5. CPU-only and GPU-enabled operation

CPU-only mode is explicit and safe on hosts without CUDA:

```bash
cd /home/runner/work/overhauled/overhauled
./build/broker_server 9090 --cpu-only
```

GPU-enabled mode uses detected topology automatically:

```bash
cd /home/runner/work/overhauled/overhauled
./build/broker_server 9090
```

Behavior notes:

- `--cpu-only` disables GPU topology detection and always reports `selected_gpu = -1`.
- Without `--cpu-only`, the broker uses GPU topology if GPUs are present.
- NVLink is an optimization, not a requirement. On non-NVLink systems the placement logic still runs and falls back to PCIe / least-loaded choices.
- The current broker executable accepts only `[port] [--cpu-only] [--help]`.

## 6. Binding, listening, and firewall guidance

Current implementation details:

- listener address: all interfaces (`0.0.0.0`)
- default port: `9090` if omitted
- socket timeout: 5000 ms
- concurrent client cap: 128 by default

Because the broker binds all interfaces, protect it with host and network policy. Example with illustrative values:

```bash
# allow only the authenticated proxy or mesh ingress address
sudo ufw allow from 192.0.2.10 to any port 9090 proto tcp
sudo ufw deny 9090/tcp
```

If you cannot restrict the port at the host firewall, place the process on a private network segment reachable only by your proxy/service-mesh layer.

## 7. Starting the process safely

Foreground, for initial verification:

```bash
cd /home/runner/work/overhauled/overhauled
./build/broker_server 9090 --cpu-only
```

Illustrative background example without inventing a service manager:

```bash
cd /home/runner/work/overhauled/overhauled
nohup ./build/broker_server 9090 --cpu-only \
  >/var/log/overhauled/broker.stdout.log \
  2>/var/log/overhauled/broker.stderr.log &
echo $! >/var/lib/overhauled/broker_server.pid
```

Use your standard supervisor/process-control tooling if you already have one; adapt ownership, logging, and restart policy to local standards.

## 8. Put it behind authenticated transport

`broker_server` speaks raw TCP and performs protocol validation only. For production, terminate identity and encryption **before** traffic reaches the broker:

- mTLS sidecar or service mesh
- TCP reverse proxy/load balancer with client authentication
- private network plus authenticated gateway

Operational expectation:

- YuKKi-OS should open TCP connections only to the authenticated proxy/gateway address.
- The proxy/gateway should forward the raw broker byte stream unchanged to `broker_server`.
- Do not insert HTTP framing or application-layer translation unless you are deliberately implementing a separate protocol adapter.

## 9. YuKKi-OS interoperability expectations

YuKKi-OS should treat overhauled as an external broker target and send versioned `BRK1` / v1 requests. In practice:

- use a stable `source` value per caller/tenant/session grouping if you want sticky GPU affinity to be meaningful
- set `destination` to the logical overhauled target your control plane expects
- set `kind` to the operation class your workload understands
- keep `timeout_ms > 0`
- keep frame, payload, and string sizes within broker limits
- treat `selected_gpu = -1` as CPU fallback rather than an error by itself

Current repository behavior to account for:

- malformed or oversized frames receive a structured `Rejected` response when possible
- the default compute path used by repository tests expects a payload that decodes as a packed `double[]` and returns doubled values
- payload semantics above framing/validation are workload-specific and should be versioned between YuKKi-OS and the service using the broker
- YuKKi-OS async clients may pipeline requests over one TCP connection and correlate responses by `task_id`; broker enforces a bounded per-connection in-flight limit (`max_inflight_per_client`, default `32`).
- If a YuKKi-OS client still emits JSON `BrokerTask` / `BrokerResult` request bodies, add a versioned adapter before the broker because `broker_server` accepts BRK1/v1 binary frames only.

## 10. Verification, health, and logging

What exists today:

- startup log to stdout, including port and optional `cpu-only mode`
- clean shutdown log to stdout
- startup failure message to stderr
- no native HTTP health endpoint
- no built-in metrics endpoint

Safe verification steps:

1. Check the binary advertises its supported CLI:

```bash
cd /home/runner/work/overhauled/overhauled
./build/broker_server --help
```

2. Run broker-related tests after building:

```bash
cd /home/runner/work/overhauled/overhauled/build
ctest --output-on-failure -R 'broker_(protocol|service|server_e2e|async_latency_benchmark)_test'
```

3. Confirm a listener exists on the chosen port (illustrative port `9090`):

```bash
ss -ltnp | grep ':9090'
```

4. Review stdout/stderr capture from your shell or supervisor.

## 11. Upgrades and rollback

Conservative sequence:

1. build the new tree in a separate build directory
2. run the broker tests in that build
3. stop existing `broker_server`
4. switch the deployed binary or install prefix
5. start the new process and verify listener/tests/logs

Rollback is simply the inverse: stop the new binary, restore the previous known-good binary/install prefix, and restart. Because the broker contract is a versioned wire protocol rather than an embedded library dependency, coordinate any protocol changes with the external control plane before rollout.

## 12. Troubleshooting

### `broker_server --help` or startup fails

- verify you built `BUILD_BROKER=ON` (default)
- verify the binary exists at `/home/runner/work/overhauled/overhauled/build/broker_server` or your install prefix

### CPU-only mode works, GPU-enabled mode does not

- rebuild with `-DENABLE_CUDA=ON`
- confirm the CUDA toolkit/runtime is visible to CMake and the host can enumerate GPUs
- use CPU-only mode as a safe temporary fallback while fixing the GPU stack

### Broker is reachable but YuKKi-OS requests are rejected

- confirm YuKKi-OS is sending `BRK1` / version `1` frames
- confirm `task_id`, `source`, `destination`, and `kind` are non-empty
- confirm `timeout_ms` is non-zero
- confirm frame/payload/string lengths are within bounds

### Connections close unexpectedly

- check that the upstream proxy forwards raw TCP bytes unchanged
- check for idle or stalled clients hitting the 5 second socket timeout
- check whether you exceeded the default 128 concurrent client limit

### Results are successful but `selected_gpu = -1`

- that is expected in `--cpu-only` mode
- in non-CPU-only mode, it also means no usable GPU topology was initialized on startup
