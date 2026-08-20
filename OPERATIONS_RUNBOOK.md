# Production Operations Runbook

## Quick Start

### Day 1: Initial Deployment

```bash
# 1. Provision hardware
# 2. Install NVIDIA stack
bash scripts/install_nvidia_stack.sh

# 3. Build
# See DEPLOYMENT_GUIDE.md: Hardware Requirements section

# 4. Install NVIDIA stack
bash scripts/install_nvidia_stack.sh

# 5. Build the server
mkdir build && cd build
cmake -DTENSORRT_ROOT=/usr/local/tensorrt -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# 4. Validate
python3 ../tests/test_protocol.py
# 4. Run validation tests
python3 ../tests/test_protocol.py
python3 ../tests/test_model_loading.py
python3 ../tests/benchmark_comprehensive.py

# 5. Start server
./gpu_runtime_server 8080 &

# 6. Monitor startup
journalctl -u overhauled -f
```

### Daily Operations

```bash
# Check server status
sudo systemctl status overhauled

# Monitor GPU health
nvidia-smi -l 1

# View performance metrics
curl http://localhost:9090/metrics

# Check logs for errors
tail -f /var/log/overhauled/server.log | grep ERROR

# Monitor queue depth
jq '.gpu_stats' /var/run/overhauled/metrics.json
```

---

## Incident Response

### High Latency Alert (P99 > 50ms)

**Detection:**
```
Alert: overhauled_latency_p99_ms > 50
```

**Response Steps:**

1. **Check GPU status (5 min)**
   ```bash
   nvidia-smi
   # Look for: throttling, high memory usage, errors
   ```

2. **Analyze queue depth (5 min)**
   ```bash
   curl http://localhost:9090/metrics | grep queue_depth
   # If queue_depth > 100, scale up
   ```

3. **Review recent changes (10 min)**
   ```bash
   git log --oneline -n 20
   # Check for recent model deployments or config changes
   ```

4. **Restart if necessary (5 min)**
   ```bash
   sudo systemctl restart overhauled
   # Monitor recovery: tail -f /var/log/overhauled/server.log
   ```

5. **Escalate if persistent (>15 min)**
   - Page on-call engineer
   - Collect diagnostics bundle
   - Failover to backup server

**Success Criteria:**
- P99 latency returns to <20ms within 15 minutes
- No data loss
- <5% error rate increase

---

### Out of Memory Error

**Detection:**
```
Error: CUDA out of memory (batch size: 32, model: 101)
```

**Response Steps:**

1. **Immediate action (2 min)**
   ```bash
   # Reduce active batch sizes
   kill -SIGUSR1 $(pgrep gpu_runtime_server)
   # Signal handler reduces batch sizes to 1
   ```

2. **Restart with reduced batch (5 min)**
   ```bash
   sudo systemctl stop overhauled
   # Edit config: batch_size_heuristic: "static", static_batch_size: 4
   sudo systemctl start overhauled
   ```

3. **Scale up GPU or reduce models (15 min)**
   - Option A: Move large models to dedicated GPU
   - Option B: Unload infrequently used models
   - Option C: Add GPU to the pool

**Prevention:**
- Monitor GPU memory continuously
- Set alert threshold at 85% utilization
- Pre-load memory test at startup

---

### Network Connectivity Issue

**Detection:**
```
Warning: 50% of inbound connections rejected (max reached)
```

**Response Steps:**

1. **Verify network (3 min)**
   ```bash
   ping -c 10 load-balancer.internal
   iftop -i eth0  # Check for saturation
   netstat -an | grep ESTABLISHED | wc -l  # Count connections
   ```

2. **Increase limits (5 min)**
   ```bash
   sysctl -w net.core.somaxconn=65535
   sysctl -w net.ipv4.tcp_max_syn_backlog=65535
   ```

3. **Restart HAProxy if using (2 min)**
   ```bash
   sudo systemctl restart haproxy
   ```

4. **Add load balancer capacity**
   - Spin up additional LB instance
   - Update DNS records

---

## Scaling Operations

### Vertical Scaling (Add GPUs to Existing Server)

**Pre-check:**
```bash
# Verify server specs
lscpu
lsmem
df -h

# Check current utilization
watch -n 1 nvidia-smi
```

**Procedure:**
1. Stop the server gracefully
   ```bash
   sudo systemctl stop overhauled
   # Wait for connections to drain (max 60s)
   ```

2. Physically install new GPU
   - Power down system
   - Install GPU
   - Verify in BIOS
   - Power on

3. Verify installation
   ```bash
   nvidia-smi
   # Should show all GPUs
   ```

4. Restart server
   ```bash
   sudo systemctl start overhauled
   # Server auto-detects all GPUs
   ```

**Validation:**
```bash
# Run benchmark
python3 tests/benchmark_comprehensive.py
# Verify throughput increased by ~3200 tasks/sec per GPU
```

### Horizontal Scaling (Add Server Nodes)

**Procedure:**
1. Set up new server (follow DEPLOYMENT_GUIDE.md)
2. Add to load balancer
   ```bash
   # Update HAProxy config
   vi /etc/haproxy/haproxy.cfg
   # Add: server srv4 server4.internal:8080 check
   
   sudo systemctl reload haproxy
   ```
3. Verify health checks
   ```bash
   curl -I http://server4.internal:8080
   ```

**Expected Result:**
- Load distributed evenly
- Throughput scales linearly
- No client code changes required

---

## Backup & Recovery

### Backup Strategy

**What to backup:**
1. Model cache
2. Configuration files
3. Metrics history
4. Audit logs

**Backup frequency:**
- Configuration: Daily (or on change)
- Models: Weekly (or on new model deployment)
- Logs: Daily (7-day retention minimum)
- Metrics: Continuous (30-day retention)

**Backup script:**
```bash
#!/bin/bash
BACKUP_DIR=/backups/overhauled
DATE=$(date +%Y-%m-%d_%H-%M-%S)

mkdir -p $BACKUP_DIR/$DATE

# Backup models
tar -czf $BACKUP_DIR/$DATE/models.tar.gz /var/cache/overhauled/models/

# Backup config
cp /etc/overhauled/*.yaml $BACKUP_DIR/$DATE/

# Backup logs (last 7 days)
find /var/log/overhauled -mtime -7 -exec cp {} $BACKUP_DIR/$DATE/ \;

echo "Backup completed: $BACKUP_DIR/$DATE"
```

### Recovery Procedure

**Recover from backup:**
```bash
BACKUP_DIR=/backups/overhauled/2024-08-20_14-30-00

# Stop server
sudo systemctl stop overhauled

# Restore models
tar -xzf $BACKUP_DIR/models.tar.gz -C /

# Restore config
cp $BACKUP_DIR/*.yaml /etc/overhauled/

# Start server
sudo systemctl start overhauled

# Verify
curl -I http://localhost:8080
```

**Time to Recovery (RTO):** ~5 minutes
**Recovery Point Objective (RPO):** <1 hour

---

## Maintenance Windows

### Monthly Maintenance

```bash
# 1. Run full diagnostics
python3 tests/benchmark_comprehensive.py
python3 tests/heuristic_analyzer.py

# 2. Update dependencies
sudo apt-get update && sudo apt-get upgrade

# 3. Review and archive logs
find /var/log/overhauled -mtime +30 -exec gzip {} \;

# 4. Analyze performance trends
grep 'latency_p99' /var/log/overhauled/*.log | avg_p99
```

### Quarterly Maintenance

```bash
# 1. Update GPU drivers
sudo apt-get install --upgrade nvidia-driver-550

# 2. Update CUDA/TensorRT
# Follow DEPLOYMENT_GUIDE.md: NVIDIA Stack Installation

# 3. Disaster recovery drill
# Backup all data
# Simulate failure
# Restore from backup
# Document time and issues

# 4. Capacity planning
# Review throughput trends
# Forecast scaling needs
# Budget for Q4
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

- [ ] GPU clock frequency locked (no throttling)
- [ ] Persistence mode enabled
- [ ] TF32 precision enabled
- [ ] Kernel fusion enabled
- [ ] Memory pool pre-allocated
- [ ] Batch size optimized (4-8 recommended)
- [ ] Streams per GPU tuned (32+ for <5ms targets)
- [ ] Network buffer sizes optimized
- [ ] CPU pinning configured

---

## Escalation Contacts

```
Level 1: On-Call Engineer
  - Slack: #overhauled-alerts
  - PagerDuty: oncall-gpu
  - Response Time: 15 minutes

Level 2: GPU Infrastructure Team
  - Slack: #gpu-infrastructure
  - Email: gpu-team@company.com
  - Response Time: 30 minutes

Level 3: NVIDIA Support
  - Phone: +1-408-486-2000
  - Portal: https://support.nvidia.com
  - Response Time: 4 hours (enterprise SLA)
```

---

## Communication Templates

### Incident Notification

```
SUBJECT: [INCIDENT] Overhauled GPU Runtime - High Latency

Severity: SEV2 (Degraded Performance)
Start Time: 2024-08-20 14:32 UTC
Affected Services: Tensor Inference API
Expected Duration: 30 minutes

Description:
The Overhauled GPU Runtime is experiencing elevated latency (P99: 50ms vs normal 12ms).
This is affecting approximately 15% of inference requests.

Root Cause: GPU memory utilization at 92% (threshold: 85%)

Mitigation:
- Reduced batch size to 4
- Unloaded 2 infrequently used models
- Added GPU node to cluster

Next Update: 14:45 UTC
```

### Recovery Notification

```
SUBJECT: [RESOLVED] Overhauled GPU Runtime - High Latency

Severity: RESOLVED
Resolution Time: 2024-08-20 14:52 UTC
Total Duration: 20 minutes

Final Status:
- All services fully operational
- Latency P99: 11ms (normal)
- GPU utilization: 78%
- Zero data loss

Root Cause Analysis to follow within 48 hours.
Post-mortem meeting: Wednesday 10:00 AM UTC
```

---

**Last Updated**: August 20, 2024
**Document Owner**: GPU Infrastructure Team
