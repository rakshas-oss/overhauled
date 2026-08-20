# Overhauled GPU Runtime - Production Deployment Guide (v0.3)

This guide is the canonical deployment and operations reference for Overhauled v0.3. It consolidates installation, configuration, deployment strategies, tuning, and troubleshooting for production and development environments.

Table of contents
1. Architecture Overview
2. Prerequisites
3. Installation & Build (checkout v0.3)
4. Configuration (config.yaml example)
5. Run & Verify (v0.3)
6. Deployment Strategies (single server, multi-GPU, load-balanced, Kubernetes)
7. Performance Tuning
8. Monitoring & Observability
9. Troubleshooting
10. Scaling Guidelines

---

## 1. Architecture Overview

Components:
- Client Applications (Python / C++ / other) — speak the ADI binary protocol over TCP
- GPU Runtime Server (gpu_runtime_server)
  - Listener / acceptor thread
  - Session threads (one per client)
  - Placer & GpuTopology (placement policy)
  - GPU worker threads (one or more per GPU)
  - Model Manager (ONNX loading, TensorRT compilation)
- NVIDIA stack: CUDA runtime, cuBLAS/cuDNN, TensorRT

How it fits together:
Clients send inference requests to the GPU Runtime Server via TCP; the server parses the ADI protocol, looks up the client's home GPU (sticky affinity), runs placement logic (Placer) based on topology (GpuTopology), and dispatches tasks to GPU worker queues. The Model Manager handles model loading and TensorRT engine lifecycle.

---

## 2. Prerequisites

Hardware (minimum):
- GPU: NVIDIA Turing-class or newer (e.g., T4, V100, RTX 20/30/40 series)
- VRAM: 4GB per GPU (8GB+ recommended)
- CPU: 4 cores (8+ recommended)
- RAM: 8GB (16GB+ recommended)
- Network: 1Gbps (10Gbps recommended for high throughput)

Hardware (recommended production):
- GPU: NVIDIA A100 / L40S
- VRAM: 24–80GB
- CPU: 16+ cores
- RAM: 32GB+
- NVMe SSD for model cache

Software
- OS: Ubuntu 20.04 LTS or later (or comparable Linux)
- CUDA: 11.8+ (minimum). CUDA 12.0 is recommended and tested for v0.3.
- TensorRT: 8.5+ (compatible with chosen CUDA version). 8.6+ recommended for CUDA 12.
- CMake: 3.18+
- Compiler: GCC 9+ (or Clang compatible with C++17)
- Python 3.8+ (for clients, examples, and benchmarking)

Python client dependencies (example):
- torch>=1.12.0 (optional, for example clients)
- onnx>=1.12.0
- numpy>=1.21.0

---

## 3. Installation & Build (checkout v0.3)

Clone and checkout the v0.3 release tag before building:

```bash
git clone https://github.com/rakshas-oss/overhauled.git
cd overhauled
# Checkout the release tag (v0.3)
git fetch --tags origin
git checkout v0.3
```

Install NVIDIA stack (example notes)
- Choose CUDA/TensorRT installers that match the recommended versions above.
- Use your distribution's packaging or the NVIDIA installers. Example (optional):

```bash
# Optional: download CUDA 12.0 local installer (example)
wget https://developer.download.nvidia.com/compute/cuda/12.0.0/local_installers/cuda_12.0.0_525.60.13_linux.run
sudo sh cuda_12.0.0_525.60.13_linux.run
# Install cuDNN / TensorRT per NVIDIA instructions
```

Build from source (example):

```bash
# from repository root (after checkout v0.3)
mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DTENSORRT_ROOT=/usr/local/tensorrt \
  -DCMAKE_CUDA_ARCHITECTURES=75
make -j$(nproc)
```

Notes:
- Replace `-DCMAKE_CUDA_ARCHITECTURES=75` with the arch code(s) for your GPUs or use a comma-separated list (e.g., 75,80).
- If you rely on system package managers (conan/vcpkg/homebrew), follow packaging/README.md and docs/PUBLISHING.md for packaging instructions.

---

## 4. Configuration (config.yaml example)

A single canonical `config.yaml` maintained next to the server binary is recommended. Minimal example:

```yaml
server:
  port: 8080
  listen_address: "0.0.0.0"
  max_connections: 1000
  thread_pool_size: 64

gpu:
  # Optional: omit device_ids to auto-detect
  device_ids: [0,1,2,3]
  streams_per_gpu: 16
  memory_fraction: 0.9

models:
  cache_dir: "/var/cache/overhauled/models"
  max_cached_models: 10
  model_timeout_seconds: 300

performance:
  enable_tensor_rt_profiling: false
  enable_kernel_fusion: true
  backlog_threshold: 32  # Placer rebalancing threshold (default)

networking:
  socket_buffer_size: 4194304  # 4MB
  tcp_nodelay: true
  keepalive_enabled: true
  keepalive_idle_seconds: 300

monitoring:
  enable_metrics: true
  metrics_port: 9090
  log_level: "INFO"
  log_file: "/var/log/overhauled/server.log"
```

Environment variables (recommended):

```bash
export CUDA_VISIBLE_DEVICES=0,1,2,3
export CUDA_LAUNCH_BLOCKING=0
export NVIDIA_TF32=1
export OMP_NUM_THREADS=16
```

Runtime parameters (CLI overrides):

```bash
./gpu_runtime_server \
  --port 8080 \
  --gpu-devices 0,1,2,3 \
  --streams-per-gpu 32 \
  --max-connections 500 \
  --log-level DEBUG
```

---

## 5. Run & Verify (v0.3)

Start the server and verify the version:

```bash
# Start server in background
./gpu_runtime_server --port 8080 &
# Check version (should report v0.3)
./gpu_runtime_server --version
# Example test client
python3 examples/test_tensor_client.py 127.0.0.1 8080
```

Expected: `Overhauled GPU Runtime Server v0.3` printed by `--version`.

---

## 6. Deployment Strategies

Strategy: keep it simple; choose the minimal architecture that meets your availability and throughput needs.

A. Single server (development / test)
- 1 instance, 1–2 GPUs, <100 clients
- Start: `./gpu_runtime_server 8080`

B. Multi-GPU single server (small production)
- 1 instance, 4–8 GPUs, 100–1000 clients
- Use `CUDA_VISIBLE_DEVICES` and `--streams-per-gpu` to tune

C. Load-balanced multi-server (production)
- HAProxy or similar load balancer distributing TCP connections across stateless server instances
- Example HAProxy backend config (TCP mode):

```
frontend tensor_inference
  bind *:8080
  mode tcp
  default_backend servers

backend servers
  mode tcp
  balance roundrobin
  server srv1 server1:8080 check
  server srv2 server2:8080 check
  server srv3 server3:8080 check
```

D. Kubernetes (hyperscale)
- Use nodeSelectors and device plugin to request GPUs (nvidia.com/gpu)
- Example deployment (cleaned and using v0.3 image):

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: overhauled-gpu-runtime
spec:
  replicas: 3
  selector:
    matchLabels:
      app: overhauled
  template:
    metadata:
      labels:
        app: overhauled
    spec:
      nodeSelector:
        gpu: "nvidia"
      containers:
      - name: gpu-runtime-server
        image: rakshas-oss/overhauled:v0.3
        args: ["--port", "8080"]
        ports:
        - containerPort: 8080
          name: tensor-inference
        - containerPort: 9090
          name: metrics
        resources:
          requests:
            nvidia.com/gpu: 4
            memory: "32Gi"
            cpu: "16"
          limits:
            nvidia.com/gpu: 4
            memory: "32Gi"
            cpu: "16"
        env:
        - name: CUDA_VISIBLE_DEVICES
          value: "0,1,2,3"
        livenessProbe:
          tcpSocket:
            port: 8080
          initialDelaySeconds: 30
          periodSeconds: 10
        readinessProbe:
          tcpSocket:
            port: 8080
          initialDelaySeconds: 20
          periodSeconds: 5
```

Apply with:

```bash
kubectl apply -f deployment.yaml
kubectl get pods -l app=overhauled
kubectl logs -f <pod-name>
```

---

## 7. Performance Tuning

Batching heuristic:
- Use small batches (4–8) for throughput-sensitive workloads with low latency impact.
- Dynamically adjust batch sizes based on queue_depth.

GPU streams and memory:
- Streams per GPU: 8–32 depending on workload
- memory_fraction: typically 0.8–0.95 depending on multi-model needs

Kernel / network optimizations:
- Enable TensorRT kernel fusion and profiling for offline tuning
- Tune OS TCP buffer sizes for high throughput (sysctl net.core.rmem_max/wmem_max)

Example sysctl tuning:

```bash
sudo sysctl -w net.core.rmem_max=134217728
sudo sysctl -w net.core.wmem_max=134217728
```

---

## 8. Monitoring & Observability

Expose Prometheus metrics on `metrics_port` (default 9090). Example quick check:

```bash
curl http://localhost:9090/metrics | grep overhauled_
```

Key metrics:
- overhauled_total_inferences
- overhauled_inference_latency_ms (quantiles)
- overhauled_gpu_utilization_percent{gpu_id}
- overhauled_queue_depth{gpu_id}

Logging: structured (JSON) logs to `/var/log/overhauled/server.log` by default. Use `log_level` in config.yaml.

---

## 9. Troubleshooting

Common issues and fixes:

CUDA Out of Memory
- Symptoms: `CUDA Error: out of memory`
- Fixes:
  - Reduce `streams_per_gpu`
  - Reduce memory_fraction in config
  - Use fewer device_ids or move to GPUs with more VRAM

High latency variance / p99 spikes
- Diagnosis: check `nvidia-smi -q` for throttling, monitor queue depths, and watch for host-staging in logs
- Fixes:
  - Increase batch size slightly or enable NVLink-aware placement (enabled by default in v0.3)
  - Ensure `GpuTopology::enable_peer_access()` is called during startup (server does this in v0.3)

Low GPU utilization
- Fixes:
  - Increase `streams_per_gpu`
  - Enable client-side batching
  - Ensure models are pre-warmed or cached

Connection timeouts
- Fixes:
  - Increase socket buffers (sysctl)
  - Increase server `--max-connections`
  - Ensure HAProxy / load balancer timeouts are configured for long-running connections

---

## 10. Scaling Guidelines

Throughput (baseline per GPU): ~3200 tasks/sec (varies by model & payload)
Scaling is near-linear; measure at your payload and concurrency.

Recommended tests before scaling production:
- Run multi-client benchmarks from docs/BENCHMARK_RESULTS.md
- Validate topology with `nvidia-smi topo -m`
- Verify `./gpu_runtime_server --version` reports v0.3 in the deployed image

---

If you want, I can also:
- Run a YAML linter on the Kubernetes snippet and commit any minor formatting fixes.
- Update packaging manifests (conanfile.py / vcpkg.json) to set the version-string to 0.3.0.

