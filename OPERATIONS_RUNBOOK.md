# Production Operations Runbook

## Quick Start

### Day 1: Initial Deployment

```bash
# 1. Provision hardware
# 2. Install NVIDIA stack
bash scripts/install_nvidia_stack.sh

# 3. Build
mkdir build && cd build
cmake -DTENSORRT_ROOT=/usr/local/tensorrt -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# 4. Validate
python3 ../tests/test_protocol.py
python3 ../tests/benchmark_comprehensive.py

# 5. Start server
./gpu_runtime_server 8080 &

# 6. Monitor
journalctl -u overhauled -f
```

### Daily Operations

```bash
# Check status
sudo systemctl status overhauled

# Monitor GPUs
nvidia-smi -l 1

# Check metrics
curl http://localhost:9090/metrics

# View error logs
tail -f /var/log/overhauled/server.log | grep ERROR
```

---

## Incident Response

### High Latency Alert (P99 > 50ms)

**Detection:**
```
Alert: overhauled_latency_p99_ms > 50
```

**Response (5-15 min to resolution):**

1. **Check GPU** (5 min)
   ```bash
   nvidia-smi
   # Check for: throttling, high memory, errors
   ```

2. **Analyze queue** (5 min)
   ```bash
   curl http://localhost:9090/metrics | grep queue_depth
   ```

3. **Review changes** (10 min)
   ```bash
   git log --oneline -n 20
   ```

4. **Restart if needed** (5 min)
   ```bash
   sudo systemctl restart overhauled
   tail -f /var/log/overhauled/server.log
   ```

5. **Escalate if >15 min** (>15 min)
   - Page on-call engineer
   - Failover to backup

**Success Criteria:**
- P99 < 20ms within 15 minutes
- No data loss
- <5% error rate increase

---

### Out of Memory Error

**Detection:**
```
Error: CUDA out of memory (batch_size: 32)
```

**Response (5-15 min to resolution):**

1. **Immediate** (2 min)
   ```bash
   kill -SIGUSR1 $(pgrep gpu_runtime_server)
   ```

2. **Reduce batch size** (5 min)
   ```bash
   sudo systemctl stop overhauled
   # Edit config: static_batch_size: 4
   sudo systemctl start overhauled
   ```

3. **Scale up** (15 min)
   - Move large models to dedicated GPU
   - Unload infrequently used models
   - Add GPU to pool

**Prevention:**
- Monitor GPU memory continuously
- Alert at 85% utilization
- Pre-test memory at startup

---

### Network Connectivity Issue

**Detection:**
```
Warning: 50% of connections rejected (max reached)
```

**Response (3-10 min to resolution):**

1. **Verify** (3 min)
   ```bash
   ping -c 10 load-balancer.internal
   iftop -i eth0
   netstat -an | grep ESTABLISHED | wc -l
   ```

2. **Tune limits** (5 min)
   ```bash
   sysctl -w net.core.somaxconn=65535
   sysctl -w net.ipv4.tcp_max_syn_backlog=65535
   ```

3. **Restart LB** (2 min)
   ```bash
   sudo systemctl restart haproxy
   ```

---

## Scaling Operations

### Vertical Scaling (Add GPUs)

```bash
# Pre-check
lscpu
lsmem
watch -n 1 nvidia-smi

# Stop gracefully
sudo systemctl stop overhauled  # Wait 60s for drains

# Install GPU
# Power down → Install → Verify BIOS → Power on

# Verify
nvidia-smi

# Restart
sudo systemctl start overhauled

# Validate
python3 tests/benchmark_comprehensive.py
# Verify throughput increased by ~3200 tasks/sec
```

### Horizontal Scaling (Add Servers)

```bash
# Set up new server (see DEPLOYMENT_GUIDE.md)

# Add to load balancer
vi /etc/haproxy/haproxy.cfg
# Add: server srv4 server4:8080 check

sudo systemctl reload haproxy

# Verify
curl -I http://server4:8080

# Result: Load distributed evenly, zero downtime
```

---

## Backup & Recovery

### Backup Strategy

**What to backup:**
- Models: Weekly (or on new deployment)
- Config: Daily (or on change)
- Logs: Daily (7-day retention)
- Metrics: Continuous (30-day retention)

**Backup script:**
```bash
#!/bin/bash
BACKUP_DIR=/backups/overhauled
DATE=$(date +%Y-%m-%d_%H-%M-%S)

mkdir -p $BACKUP_DIR/$DATE

tar -czf $BACKUP_DIR/$DATE/models.tar.gz /var/cache/overhauled/models/
cp /etc/overhauled/*.yaml $BACKUP_DIR/$DATE/
find /var/log/overhauled -mtime -7 -exec cp {} $BACKUP_DIR/$DATE/ \;

echo "Backup completed: $BACKUP_DIR/$DATE"
```

### Recovery Procedure

```bash
BACKUP_DIR=/backups/overhauled/2024-08-20_14-30-00

sudo systemctl stop overhauled
tar -xzf $BACKUP_DIR/models.tar.gz -C /
cp $BACKUP_DIR/*.yaml /etc/overhauled/
sudo systemctl start overhauled

curl -I http://localhost:8080
```

**RTO**: 5 minutes
**RPO**: <1 hour

---

## Maintenance Windows

### Monthly

```bash
# Run diagnostics
python3 tests/benchmark_comprehensive.py
python3 tests/heuristic_analyzer.py

# Update system
sudo apt-get update && sudo apt-get upgrade

# Archive logs
find /var/log/overhauled -mtime +30 -exec gzip {} \;

# Review performance trends
grep 'latency_p99' /var/log/overhauled/*.log | stats
```

### Quarterly

```bash
# Update GPU drivers
sudo apt-get install --upgrade nvidia-driver-550

# Update CUDA/TensorRT
# (See DEPLOYMENT_GUIDE.md)

# Disaster recovery drill
# - Backup all data
# - Simulate failure
# - Restore from backup
# - Document time

# Capacity planning
# - Review throughput trends
# - Forecast scaling needs
# - Budget for Q4
```

---

## Performance Optimization

### Baseline Metrics (Update Quarterly)

```json
{
  "timestamp": "2024-08-20",
  "config": {
    "gpus": 8,
    "streams_per_gpu": 16,
    "model_count": 5
  },
  "metrics": {
    "throughput_tasks_per_sec": 25600,
    "latency_p50_ms": 5.0,
    "latency_p99_ms": 12.0,
    "queue_depth_avg": 3.2,
    "gpu_utilization_avg": 87.5,
    "memory_usage_gb": 156
  }
}
```

### Performance Tuning Checklist

- [ ] GPU clock locked (no throttling)
- [ ] Persistence mode enabled
- [ ] TF32 precision enabled
- [ ] Kernel fusion enabled
- [ ] Memory pool pre-allocated
- [ ] Batch size optimized (4-8)
- [ ] Streams per GPU tuned (32+)
- [ ] Network buffer sizes optimized
- [ ] CPU pinning configured

---

## Escalation Contacts

```
Level 1: On-Call Engineer
  - Slack: #overhauled-alerts
  - Response: 15 minutes

Level 2: GPU Infrastructure Team
  - Slack: #gpu-infrastructure
  - Response: 30 minutes

Level 3: NVIDIA Support
  - Phone: +1-408-486-2000
  - Response: 4 hours (enterprise SLA)
```

---

## Communication Templates

### Incident Notification

```
SUBJECT: [INCIDENT] Overhauled GPU Runtime - High Latency

Severity: SEV2 (Degraded Performance)
Start: 2024-08-20 14:32 UTC
Affected: Tensor Inference API
Duration: 30 minutes

Description:
Elevated latency (P99: 50ms vs normal 12ms)
Affecting 15% of requests

Root Cause: GPU memory at 92% (threshold: 85%)

Mitigation:
- Reduced batch size to 4
- Unloaded 2 models
- Added GPU node

Next Update: 14:45 UTC
```

### Recovery Notification

```
SUBJECT: [RESOLVED] Overhauled GPU Runtime - High Latency

Status: RESOLVED
Resolution: 2024-08-20 14:52 UTC
Duration: 20 minutes

Final Status:
- All services operational
- Latency P99: 11ms
- GPU utilization: 78%
- Zero data loss

Post-mortem: Wednesday 10:00 AM UTC
```

---

**Version**: 1.0.0
**Last Updated**: August 20, 2024
