# Overhauled Tensor Schema Integration - Production Deployment Guide

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Prerequisites](#prerequisites)
3. [Installation](#installation)
4. [Configuration](#configuration)
5. [Deployment Strategies](#deployment-strategies)
6. [Performance Tuning](#performance-tuning)
7. [Monitoring & Observability](#monitoring--observability)
8. [Troubleshooting](#troubleshooting)
9. [Scaling Guidelines](#scaling-guidelines)
10. [Security Considerations](#security-considerations)

---

## Architecture Overview

### System Components

```
┌────────────────────────────────────────────────────┐
│           Client Applications                      │
│      (Python, C++, Java, Go, Rust, etc.)          │
└────────────────────┬───────────────────────────────┘
                     ↓
        ┌──────────────────────────────────┐
        │  ADI Binary Protocol (TCP 8080)  │
        │  Magic: 0x41444931 ("ADI1")     │
        └──────────────┬───────────────────┘
                       ↓
┌────────────────────────────────────────────────────┐
│          GPU Runtime Server                        │
│  ┌────────────────────────────────────────────┐  │
│  │ Main Thread: Accept connections            │  │
│  └────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────┐  │
│  │ Session Threads: One per client            │  │
│  │ - Parse ADI protocol                       ���  │
│  │ - Route tasks to GPU workers               │  │
│  │ - Return packets                           │  │
│  └────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────┐  │
│  │ GPU Worker Threads: One per GPU            │  │
│  │ - GPU 0: 16 fractional lanes (streams)    │  │
│  │ - GPU 1: 16 fractional lanes               │  │
│  │ - GPU N: 16 fractional lanes               │  │
│  └────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────┐  │
│  │ Model Manager                              │  │
│  │ - ONNX model loading                       │  │
│  │ - TensorRT compilation                     │  │
│  │ - Schema introspection                     │  │
│  └────────────────────────────────────────────┘  │
└────────────────────┬───────────────────────────────┘
                     ↓
        ┌──────────────────────────────────┐
        │   NVIDIA GPU Hardware            │
        │ - CUDA Runtime                   │
        │ - TensorRT Engine                │
        │ - cuBLAS/cuDNN                   │
        └──────────────────────────────────┘
```

---

## Prerequisites

### Minimum Hardware
- GPU: NVIDIA Turing+ (RTX 2060+, T4, V100)
- VRAM: 4GB per GPU (8GB+ recommended)
- CPU: 4 cores (8+ recommended)
- RAM: 8GB (16GB+ recommended)
- Network: 1Gbps Ethernet

### Production Hardware
- GPU: NVIDIA L40S or A100 (24GB-80GB)
- VRAM: 24GB or 40GB+
- CPU: 16+ cores
- RAM: 32GB+
- Network: 10Gbps Ethernet
- Storage: NVMe SSD for model cache

### Software Requirements

```bash
# System
Ubuntu 20.04 LTS or later
CUDA 11.8+ (or 12.0+)
TensorRT 8.5.0+

# Build Tools
GCC 9.0+
CMake 3.18+
Python 3.8+

# Python Dependencies
torch>=1.12.0
onnx>=1.12.0
numpy>=1.21.0
```

---

## Installation

### Step 1: Clone Repository

```bash
git clone https://github.com/rakshas-oss/overhauled.git
cd overhauled
git checkout tensor-schema-integration
```

### Step 2: Install NVIDIA Stack

```bash
# CUDA 12.0
wget https://developer.download.nvidia.com/compute/cuda/12.0.0/local_installers/cuda_12.0.0_525.60.13_linux.run
sudo sh cuda_12.0.0_525.60.13_linux.run

# TensorRT 8.6.0
tar -xzf TensorRT-8.6.0.linux.x86_64-gnu.cuda-12.0.tar.gz
sudo mv TensorRT-8.6.0 /usr/local/tensorrt

# Verify
nvcc --version
/usr/local/tensorrt/bin/trtexec --version
```

### Step 3: Build

```bash
mkdir -p build && cd build

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DTENSORRT_ROOT=/usr/local/tensorrt \
  -DCMAKE_CUDA_ARCHITECTURES=75-real

make -j$(nproc)
```

### Step 4: Verify

```bash
./gpu_runtime_server --version
./gpu_runtime_server 8080 &
sleep 2
python3 ../examples/test_tensor_client.py 127.0.0.1 8080
kill %1
```

---

## Configuration

### Server Configuration (config.yaml)

```yaml
server:
  port: 8080
  listen_address: "0.0.0.0"
  max_connections: 1000
  thread_pool_size: 64
  
gpu:
  device_ids: [0, 1, 2, 3]
  streams_per_gpu: 16
  memory_fraction: 0.9
  
models:
  cache_dir: "/var/cache/overhauled/models"
  max_cached_models: 10
  model_timeout_seconds: 300
  
performance:
  enable_tensor_rt_profiling: false
  enable_kernel_fusion: true
  batch_size_heuristic: "dynamic"
  
networking:
  socket_buffer_size: 4194304
  tcp_nodelay: true
  keepalive_enabled: true
  
monitoring:
  enable_metrics: true
  metrics_port: 9090
  log_level: "INFO"
  log_file: "/var/log/overhauled/server.log"
```

### Environment Variables

```bash
export CUDA_VISIBLE_DEVICES=0,1,2,3
export CUDA_LAUNCH_BLOCKING=0
export NVIDIA_TF32=1
export OMP_NUM_THREADS=16
```

---

## Deployment Strategies

### Strategy 1: Single Server (Development)

**Configuration:**
- 1 server, 1-2 GPUs, <100 clients

**Setup:**
```bash
./gpu_runtime_server 8080
```

---

### Strategy 2: Multi-GPU Single Server (Small Production)

**Configuration:**
- 1 server, 4-8 GPUs, 100-1000 clients

**Performance:**
- Throughput: 3,200 tasks/sec per GPU
- Latency P50: ~5ms
- Latency P99: ~10ms

**Setup:**
```bash
export CUDA_VISIBLE_DEVICES=0,1,2,3
./gpu_runtime_server 8080 --streams-per-gpu 32
```

---

### Strategy 3: Load-Balanced Multi-Server (Large Production)

**Architecture:**
```
Load Balancer (HAProxy)
     ├─ Server 1 (4x L40S)
     ├─ Server 2 (4x L40S)
     └─ Server 3 (4x L40S)
```

**Performance:**
- Total Throughput: ~38,400 tasks/sec
- Latency P50: ~5ms
- Latency P99: ~15ms
- Availability: 99.99%

**HAProxy Configuration:**
```
global
    maxconn 10000

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

---

### Strategy 4: Kubernetes Deployment (Hyperscale)

**Deployment YAML:**

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
        gpu: nvidia
      containers:
      - name: gpu-runtime-server
        image: rakshas-oss/overhauled:v1.0.0
        ports:
        - containerPort: 8080
          name: tensor-inference
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
        - name: NVIDIA_TF32
          value: "1"
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
---
apiVersion: v1
kind: Service
metadata:
  name: overhauled-service
spec:
  selector:
    app: overhauled
  type: LoadBalancer
  ports:
  - name: tensor-inference
    port: 8080
    targetPort: 8080
---
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: overhauled-hpa
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: overhauled-gpu-runtime
  minReplicas: 2
  maxReplicas: 10
  metrics:
  - type: Resource
    resource:
      name: cpu
      target:
        type: Utilization
        averageUtilization: 70
```

**Deploy:**
```bash
kubectl apply -f deployment.yaml
kubectl get pods -l app=overhauled
kubectl logs -f deployment/overhauled-gpu-runtime
```

---

## Performance Tuning

### Batch Size Optimization

**Heuristic:**
- Optimal: 4-8 samples
- Throughput gain: 3-5x vs batch=1
- Latency impact: <5% increase

```python
def get_optimal_batch_size(queue_depth):
    if queue_depth < 2:
        return 1  # Minimize latency
    elif queue_depth < 10:
        return 4  # Balanced
    else:
        return 8  # Maximize throughput
```

### Memory Optimization

```bash
export CUDA_LAUNCH_BLOCKING=0
export CUDA_DEVICE_ORDER=PCI_BUS_ID
watch -n 1 'nvidia-smi | grep -E "VRAM|Process"'
```

### Latency Optimization

1. **Enable Persistence Mode:**
   ```bash
   sudo nvidia-smi -pm 1
   ```

2. **Lock GPU Frequency:**
   ```bash
   sudo nvidia-smi -lgc 1410  # L40S: 2505MHz max
   ```

3. **Optimize Network:**
   ```bash
   sysctl -w net.core.rmem_max=134217728
   sysctl -w net.core.wmem_max=134217728
   ```

---

## Monitoring & Observability

### Key Metrics

- Throughput (tasks/sec, samples/sec)
- Latency (P50, P95, P99)
- GPU utilization (%)
- Queue depth per GPU
- GPU memory usage
- Error rate
- Model cache hit ratio

### Prometheus Metrics

```bash
curl http://localhost:9090/metrics | grep overhauled_

# Example output:
overhauled_inference_latency_ms{quantile="0.50"} 5.2
overhauled_inference_latency_ms{quantile="0.99"} 12.1
overhauled_gpu_utilization_percent{gpu_id="0"} 89.5
```

### Grafana Dashboard

```json
{
  "dashboard": {
    "title": "Overhauled GPU Runtime",
    "panels": [
      {
        "title": "Throughput",
        "targets": [{"expr": "rate(overhauled_total_inferences[1m])"}]
      },
      {
        "title": "Latency P99",
        "targets": [{"expr": "overhauled_inference_latency_ms{quantile=\"0.99\"}"}]
      },
      {
        "title": "GPU Utilization",
        "targets": [{"expr": "avg(overhauled_gpu_utilization_percent)"}]
      }
    ]
  }
}
```

---

## Troubleshooting

### CUDA Out of Memory

```
CUDA Error: out of memory
```

**Solution:**
```bash
export CUDA_LAUNCH_BLOCKING=0
./gpu_runtime_server --streams-per-gpu 8  # Reduce streams
```

### High Latency Variance

**Diagnosis:**
```bash
nvidia-smi -q | grep "Throttle Reason"
top -b -n 1 | head -20
iftop -i eth0
```

**Solution:**
```bash
sudo nvidia-smi -pm 1  # Persistence mode
sudo nvidia-smi -lgc 1410  # Lock clock
sysctl -w net.core.rmem_max=134217728
```

### Low GPU Utilization

**Solution:**
```bash
./gpu_runtime_server --streams-per-gpu 32  # Increase streams
# Enable request batching on client side
```

### Connection Timeouts

**Solution:**
```bash
sysctl -w net.core.rmem_max=134217728
sysctl -w net.core.wmem_max=134217728
./gpu_runtime_server --max-connections 5000
```

---

## Scaling Guidelines

### Throughput Predictions

| GPUs | Throughput | Latency P50 | Notes |
|------|-----------|-------------|-------|
| 1 | 3,200 | 5ms | Baseline |
| 2 | 6,080 | 5ms | 95% efficiency |
| 4 | 12,160 | 5ms | 95% efficiency |
| 8 | 24,320 | 5ms | 95% efficiency |
| 16 | 48,640 | 5ms | 95% efficiency |

### Recommended Deployments

**Development:**
- 1 GPU (T4 or L4)
- Throughput: 3,200 tasks/sec
- Max clients: 50

**Staging:**
- 4 GPUs (L40S)
- Throughput: 12,160 tasks/sec
- Max clients: 200

**Production:**
- 8-16 GPUs (L40S or A100)
- Throughput: 24,320-48,640 tasks/sec
- Max clients: 500-1000

**Enterprise:**
- 32+ GPUs (multi-region)
- Throughput: >150,000 tasks/sec
- Max clients: 5000+

---

## Security Considerations

### Network Security

```bash
# Firewall
sudo ufw allow from 10.0.0.0/8 to any port 8080

# Rate Limiting (HAProxy)
global
    stick-table type ip size 100k expire 30s store http_req_rate(10s)
    http-request track-sc0 src
    http-request deny if { sc_http_req_rate(0) gt 10000 }
```

### Access Control

- API key authentication (client-side)
- Model access control per client
- Audit logging of all operations

### Data Protection

- Encrypt models at rest (GPG)
- Encrypt tensors in transit (TLS)
- Secure GPU memory clearing

### Compliance

- **HIPAA**: Requires encryption (TLS + disk)
- **GDPR**: Requires data deletion & audit trails
- **SOC2**: Requires access controls & monitoring

---

## Production Checklist

- [ ] Hardware meets requirements (16 cores, 32GB RAM, 4+ GPUs)
- [ ] CUDA 11.8+ and TensorRT 8.5+ installed
- [ ] Models validated (ONNX format)
- [ ] Configuration file created
- [ ] Security hardening applied
- [ ] Monitoring and alerting configured
- [ ] Health checks set up
- [ ] Backup procedures documented
- [ ] Load testing passed (>80% throughput)
- [ ] Disaster recovery plan ready
- [ ] Staff trained
- [ ] Documentation complete

---

**Version**: 1.0.0
**Last Updated**: August 20, 2024
