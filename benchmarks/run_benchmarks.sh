#!/bin/bash
# Comprehensive benchmark suite for NVLink placement library
# Measures placement efficiency, load balancing, and performance impact

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${DIR}/build"
BENCHMARK_DIR="${DIR}/benchmarks"
RESULTS_DIR="${DIR}/results"

echo "=== NVLink Placement Benchmark Suite ==="
echo ""

# Create directories
mkdir -p "${BUILD_DIR}" "${RESULTS_DIR}"

# Build library
echo "[1/4] Building library..."
cd "${BUILD_DIR}"
if [ ! -f CMakeCache.txt ]; then
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=ON > /dev/null
fi
make -j$(nproc) > /dev/null 2>&1
echo "✓ Build complete"

echo ""
echo "[2/4] Running minimal example (topology detection)..."
${BUILD_DIR}/example_minimal > "${RESULTS_DIR}/topology.log" 2>&1
echo "✓ Topology detection completed"
echo "Output:"
head -20 "${RESULTS_DIR}/topology.log"

echo ""
echo "[3/4] Running multi-client benchmark..."

# Monitor GPUs during benchmark
if command -v nvidia-smi &> /dev/null; then
    (
        while true; do
            nvidia-smi --query-gpu=index,utilization.gpu,utilization.memory,memory.used --format=csv,noheader,nounits
            sleep 1
        done
    ) > "${RESULTS_DIR}/gpu_stats.csv" 2>&1 &
    GPU_MONITOR_PID=$!
fi

# Run multi-client benchmark
for run in {1..3}; do
    echo "Run $run/3..."
    ${BUILD_DIR}/example_multi_client >> "${RESULTS_DIR}/multi_client_run${run}.log" 2>&1
done

# Stop GPU monitoring
if [ -n "${GPU_MONITOR_PID}" ]; then
    kill ${GPU_MONITOR_PID} 2>/dev/null || true
    sleep 1
fi

echo "✓ Multi-client benchmark completed"

echo ""
echo "[4/4] Analyzing results..."

# Extract metrics from logs
echo "" > "${RESULTS_DIR}/summary.txt"
echo "=== NVLink Placement Benchmark Summary ===" >> "${RESULTS_DIR}/summary.txt"
echo "" >> "${RESULTS_DIR}/summary.txt"

echo "GPU Topology:" >> "${RESULTS_DIR}/summary.txt"
grep "\[NVLink\]" "${RESULTS_DIR}/topology.log" >> "${RESULTS_DIR}/summary.txt"

echo "" >> "${RESULTS_DIR}/summary.txt"
echo "Multi-Client Results (3 runs):" >> "${RESULTS_DIR}/summary.txt"
for run in {1..3}; do
    if [ -f "${RESULTS_DIR}/multi_client_run${run}.log" ]; then
        echo "" >> "${RESULTS_DIR}/summary.txt"
        echo "Run $run:" >> "${RESULTS_DIR}/summary.txt"
        grep -E "Total tasks|GPU Queue|Imbalance" "${RESULTS_DIR}/multi_client_run${run}.log" >> "${RESULTS_DIR}/summary.txt" || true
    fi
done

echo "" >> "${RESULTS_DIR}/summary.txt"
echo "GPU Utilization Statistics:" >> "${RESULTS_DIR}/summary.txt"
if [ -f "${RESULTS_DIR}/gpu_stats.csv" ]; then
    awk -F, '{print $2, $3}' "${RESULTS_DIR}/gpu_stats.csv" | tail -20 >> "${RESULTS_DIR}/summary.txt"
fi

echo "✓ Analysis complete"

echo ""
echo "=== Results ==="
echo "Summary: ${RESULTS_DIR}/summary.txt"
echo "Topology: ${RESULTS_DIR}/topology.log"
echo "Multi-client runs: ${RESULTS_DIR}/multi_client_run*.log"
if [ -f "${RESULTS_DIR}/gpu_stats.csv" ]; then
    echo "GPU stats: ${RESULTS_DIR}/gpu_stats.csv"
fi

echo ""
echo "Summary:"
cat "${RESULTS_DIR}/summary.txt"
