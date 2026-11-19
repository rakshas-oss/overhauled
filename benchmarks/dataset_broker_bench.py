#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Drive Overhauled's native BRK2 broker from YuKKi-OS multivariate workloads.

The eigensolver and dataset semantics live in YuKKi-OS. This adapter measures
only the Overhauled broker ingress/dispatch path; it does not duplicate numeric
algorithms or rewrite the source datasets.

Example:
  python3 benchmarks/dataset_broker_bench.py \
    --manifest ../YuKKi-OS/examples/benchmark-datasets/manifest.json \
    --host 127.0.0.1 --port 9090 --output broker-dataset-results.json
"""
import argparse
import concurrent.futures
import hashlib
import json
import math
import pathlib
import socket
import statistics
import struct
import time
from itertools import count

MAGIC = b"BRK2"
HEADER = struct.Struct("!4sBBHHHQIHHI")
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_DATASET_BYTES = 512 * 1024 * 1024
MAX_RECORD_BYTES = 64 * 1024
MAX_RECORDS = 1_000_000
MAX_FEATURES = 64
MAX_RESPONSE_BYTES = 16 * 1024 * 1024
MAX_BATCH_SIZE = 1024
MAX_WORKERS = 32
REQUEST_IDS = count(1)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_manifest(path):
    if path.stat().st_size > MAX_MANIFEST_BYTES:
        raise ValueError("manifest exceeds 1 MiB")
    with path.open("r", encoding="utf-8") as source:
        manifest = json.load(source)
    if manifest.get("version") != 1:
        raise ValueError("unsupported dataset manifest version")
    datasets = manifest.get("datasets")
    profiles = manifest.get("profiles")
    if not isinstance(datasets, list) or not 1 <= len(datasets) <= 256:
        raise ValueError("manifest datasets must contain 1..=256 entries")
    if not isinstance(profiles, list) or not 1 <= len(profiles) <= 64:
        raise ValueError("manifest profiles must contain 1..=64 entries")
    return manifest


def load_dataset(base, spec):
    source_name = spec.get("source")
    if not isinstance(source_name, str) or not source_name:
        raise ValueError(f"dataset {spec.get('id')} has an invalid source path")
    source = pathlib.PurePath(source_name)
    if source.is_absolute() or ".." in source.parts:
        raise ValueError("dataset source must be relative and cannot traverse parent directories")
    root = base.resolve()
    try:
        path = (root / pathlib.Path(*source.parts)).resolve(strict=True)
        path.relative_to(root)
    except (OSError, ValueError) as error:
        raise ValueError("dataset source resolves outside the manifest directory or does not exist") from error
    if not path.is_file():
        raise ValueError(f"dataset {spec.get('id')} source is not a regular file")
    if path.stat().st_size > MAX_DATASET_BYTES:
        raise ValueError(f"dataset {spec.get('id')} exceeds 512 MiB")
    fields = spec.get("feature_fields")
    if not isinstance(fields, list) or not 1 <= len(fields) <= MAX_FEATURES:
        raise ValueError(f"dataset {spec.get('id')} has invalid feature fields")
    if any(not isinstance(field, str) or not field for field in fields):
        raise ValueError(f"dataset {spec.get('id')} has invalid feature names")
    if len(set(fields)) != len(fields):
        raise ValueError(f"dataset {spec.get('id')} has duplicate feature names")

    rows = []
    with path.open("rb") as source_file:
        for line_number, raw in enumerate(source_file, 1):
            if len(raw) > MAX_RECORD_BYTES:
                raise ValueError(f"dataset {spec.get('id')} row {line_number} exceeds 64 KiB")
            if not raw.strip():
                continue
            row = json.loads(raw)
            if not isinstance(row, dict):
                raise ValueError(f"dataset {spec.get('id')} row {line_number} is not an object")
            sequence = row.get(spec["sequence_field"])
            timestamp = row.get(spec["timestamp_ns_field"])
            if isinstance(sequence, bool) or not isinstance(sequence, int) or sequence < 0:
                raise ValueError(f"dataset {spec.get('id')} row {line_number} has invalid sequence")
            if isinstance(timestamp, bool) or not isinstance(timestamp, int) or timestamp < 0:
                raise ValueError(f"dataset {spec.get('id')} row {line_number} has invalid timestamp")
            values = []
            for field in fields:
                value = row.get(field)
                if isinstance(value, bool) or not isinstance(value, (int, float)):
                    raise ValueError(f"dataset {spec.get('id')} row {line_number} field {field} is not numeric")
                value = float(value)
                if not math.isfinite(value):
                    raise ValueError(f"dataset {spec.get('id')} row {line_number} field {field} is non-finite")
                values.append(value)
            rows.append((sequence, timestamp, values))
            if len(rows) > MAX_RECORDS:
                raise ValueError(f"dataset {spec.get('id')} exceeds one million records")
    if not rows:
        raise ValueError(f"dataset {spec.get('id')} contains no records")
    return path, rows


def select_sample(rows, profile):
    warmup = min(profile.get("warmup_records", 0), len(rows))
    available = rows[warmup:]
    if not available:
        raise ValueError("profile leaves no records after warmup")
    requested = profile.get("sample_records")
    if isinstance(requested, bool) or not isinstance(requested, int) or requested <= 0:
        raise ValueError("profile sample_records must be positive")
    count_rows = min(requested, len(available))
    selected = []
    for index in range(count_rows):
        relative = 0 if count_rows == 1 else index * (len(available) - 1) // (count_rows - 1)
        selected.append(available[relative])
    return rows[:warmup], selected


def tlv(field_id, kind, flags, payload):
    return struct.pack("!HBBI", field_id, kind, flags, len(payload)) + payload


def request_frame(request_id, dataset_id, record, device="cpu"):
    sequence, timestamp_ns, values = record
    payload = struct.pack("!QQ" + "d" * len(values), sequence, timestamp_ns, *values)
    body = b"".join((
        tlv(1, 8, 1, b"dataset-benchmark"),
        tlv(2, 8, 1, dataset_id.encode("utf-8")),
        tlv(3, 8, 1, b"overhauled"),
        tlv(4, 8, 1, b"inference"),
        tlv(5, 1, 1, b"\x05"),
        tlv(6, 3, 1, struct.pack("!I", 10000)),
        tlv(7, 9, 1, payload),
        tlv(10, 8, 0, device.encode("ascii")),
    ))
    header = HEADER.pack(MAGIC, 2, 0, 32, 3, 0, request_id, 1, 1, 0, len(body))
    frame = header + body
    return struct.pack("!I", len(frame)) + frame


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("broker closed connection mid-frame")
        data.extend(chunk)
    return bytes(data)


def recv_response(sock):
    size = struct.unpack("!I", recv_exact(sock, 4))[0]
    if size < HEADER.size or size > MAX_RESPONSE_BYTES:
        raise ValueError("response frame length outside benchmark safety bounds")
    return recv_exact(sock, size)


def decode_response(response, expected_request_id, expected_task_id, device_preference):
    if len(response) < HEADER.size:
        raise ValueError("truncated BRK2 response header")
    magic, major, minor, header_len, message_type, flags, request_id, schema, version, reserved, payload_len = HEADER.unpack_from(response)
    if magic != MAGIC or major != 2 or minor != 0:
        raise ValueError("invalid BRK2 response magic or version")
    if header_len != HEADER.size or message_type != 4 or not (flags & 1):
        raise ValueError("invalid BRK2 response envelope")
    if flags & ~0x0007 or reserved != 0 or schema != 2 or version != 1:
        raise ValueError("unsupported BRK2 response flags or schema")
    if request_id != expected_request_id:
        raise ValueError(f"BRK2 response correlation mismatch: expected {expected_request_id}, got {request_id}")
    if payload_len != len(response) - HEADER.size:
        raise ValueError("BRK2 response payload length mismatch")

    fields = {}
    offset = HEADER.size
    while offset < len(response):
        if len(response) - offset < 8:
            raise ValueError("truncated BRK2 response TLV header")
        field_id, wire_type, field_flags, value_len = struct.unpack_from("!HBBI", response, offset)
        offset += 8
        if field_flags & ~0x03 or value_len > len(response) - offset:
            raise ValueError("invalid or truncated BRK2 response TLV")
        if field_id in fields:
            raise ValueError(f"duplicate BRK2 response field {field_id}")
        fields[field_id] = (wire_type, response[offset:offset + value_len])
        offset += value_len

    def required(field_id, wire_type, size=None):
        item = fields.get(field_id)
        if item is None or item[0] != wire_type or (size is not None and len(item[1]) != size):
            raise ValueError(f"missing or invalid BRK2 response field {field_id}")
        return item[1]

    try:
        task_id = required(1, 8).decode("utf-8")
        status = required(2, 1, 1)[0]
        selected_gpu = struct.unpack("!q", required(3, 5, 8))[0]
        latency_ms = struct.unpack("!Q", required(4, 4, 8))[0]
        result = required(5, 9)
        error = required(6, 8).decode("utf-8")
        backend = required(7, 8).decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("invalid UTF-8 in BRK2 response") from error

    if task_id != expected_task_id:
        raise ValueError(f"BRK2 task correlation mismatch: expected {expected_task_id!r}, got {task_id!r}")
    if status != 0:
        raise RuntimeError(f"broker task failed with status={status}: {error}")
    if backend not in ("cpu", "gpu", "npu"):
        raise ValueError(f"broker reported unknown execution backend {backend!r}")
    if (backend == "gpu" and selected_gpu < 0) or (backend != "gpu" and selected_gpu != -1):
        raise ValueError("broker backend and selected_gpu telemetry disagree")
    allowed = {"cpu"} if device_preference == "cpu" else {device_preference, "cpu"}
    if backend not in allowed:
        raise ValueError(f"requested {device_preference} but broker reported {backend}")
    return {
        "backend": backend,
        "selected_gpu": selected_gpu,
        "latency_ms": latency_ms,
        "result_bytes": len(result),
        "fallback": backend != device_preference,
    }


def run_worker(host, port, dataset_id, records, batch_size, timeout, device):
    latencies = []
    backend_counts = {}
    selected_gpus = set()
    fallback_requests = 0
    with socket.create_connection((host, port), timeout=timeout) as connection:
        connection.settimeout(timeout)
        for offset in range(0, len(records), batch_size):
            batch = records[offset:offset + batch_size]
            sent_at = {}
            frames = []
            for record in batch:
                request_id = next(REQUEST_IDS)
                sent_at[request_id] = time.perf_counter_ns()
                frames.append(request_frame(request_id, dataset_id, record, device))
            connection.sendall(b"".join(frames))
            for _ in batch:
                response = recv_response(connection)
                response_id = struct.unpack_from("!Q", response, 12)[0] if len(response) >= HEADER.size else -1
                started = sent_at.pop(response_id, None)
                if started is None:
                    raise ValueError(f"unexpected or duplicate response request_id={response_id}")
                telemetry = decode_response(response, response_id, "dataset-benchmark", device)
                latencies.append(time.perf_counter_ns() - started)
                backend_counts[telemetry["backend"]] = backend_counts.get(telemetry["backend"], 0) + 1
                if telemetry["selected_gpu"] >= 0:
                    selected_gpus.add(telemetry["selected_gpu"])
                fallback_requests += int(telemetry["fallback"])
            if sent_at:
                raise ValueError(f"missing responses for request_ids={sorted(sent_at)}")
    return latencies, backend_counts, selected_gpus, fallback_requests


def run_candidate(host, port, dataset_id, records, batch_size, workers, timeout, device):
    if not records:
        raise ValueError("candidate has no records")
    active_workers = min(workers, len(records))
    partitions = [records[index::active_workers] for index in range(active_workers)]
    started = time.perf_counter()
    latencies_ns = []
    backend_counts = {}
    selected_gpus = set()
    fallback_requests = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=active_workers) as pool:
        futures = [
            pool.submit(run_worker, host, port, dataset_id, part, batch_size, timeout, device)
            for part in partitions if part
        ]
        for future in concurrent.futures.as_completed(futures):
            worker_latencies, worker_backends, worker_gpus, worker_fallbacks = future.result()
            latencies_ns.extend(worker_latencies)
            for backend, count in worker_backends.items():
                backend_counts[backend] = backend_counts.get(backend, 0) + count
            selected_gpus.update(worker_gpus)
            fallback_requests += worker_fallbacks
    elapsed = time.perf_counter() - started
    latencies_ms = sorted(value / 1_000_000 for value in latencies_ns)
    def percentile(p):
        if not latencies_ms:
            return 0.0
        index = min(len(latencies_ms) - 1, math.ceil((len(latencies_ms) - 1) * p))
        return latencies_ms[index]
    return {
        "batch_size": batch_size,
        "workers": active_workers,
        "requests": len(latencies_ns),
        "elapsed_ms": elapsed * 1000.0,
        "requests_per_second": len(latencies_ns) / max(elapsed, 1.0e-12),
        "mean_request_ms": statistics.fmean(latencies_ms) if latencies_ms else 0.0,
        "p50_request_ms": percentile(0.50),
        "p95_request_ms": percentile(0.95),
        "p99_request_ms": percentile(0.99),
        "actual_backend_counts": dict(sorted(backend_counts.items())),
        "selected_gpu_ids": sorted(selected_gpus),
        "fallback_requests": fallback_requests,
    }


def run_profile(host, port, dataset_id, rows, profile, timeout, dry_run, device):
    objective = profile.get("objective")
    if objective not in ("latency", "throughput"):
        return None
    batch_sizes = profile.get("batch_sizes")
    worker_counts = profile.get("worker_counts")
    if not isinstance(batch_sizes, list) or not batch_sizes or any(
        isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= MAX_BATCH_SIZE
        for value in batch_sizes
    ):
        raise ValueError(f"profile {profile.get('id')} has invalid batch_sizes")
    if not isinstance(worker_counts, list) or not worker_counts or any(
        isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= MAX_WORKERS
        for value in worker_counts
    ):
        raise ValueError(f"profile {profile.get('id')} has invalid worker_counts")
    warmup, sample = select_sample(rows, profile)
    if dry_run:
        return {
            "dataset_id": dataset_id,
            "profile_id": profile["id"],
            "objective": objective,
            "warmup_records": len(warmup),
            "sampled_records": len(sample),
            "feature_values_per_record": len(sample[0][2]),
            "candidates": [
                {"batch_size": batch, "workers": workers}
                for batch in batch_sizes for workers in worker_counts
            ],
            "status": "dry_run_not_measured",
        }

    if warmup:
        run_candidate(host, port, dataset_id, warmup, 1, 1, timeout, device)

    candidates = [
        run_candidate(host, port, dataset_id, sample, batch, workers, timeout, device)
        for batch in batch_sizes for workers in worker_counts
    ]
    limit = profile.get("max_p95_request_ms")
    if limit is not None:
        if isinstance(limit, bool) or not isinstance(limit, (int, float)) or not math.isfinite(float(limit)) or limit <= 0:
            raise ValueError(f"profile {profile.get('id')} has invalid max_p95_request_ms")
    eligible = [
        item for item in candidates
        if limit is None or item["p95_request_ms"] <= float(limit)
    ]
    met_latency_target = bool(eligible) or limit is None
    pool = eligible or candidates
    if objective == "latency":
        recommended = min(pool, key=lambda item: (item["p95_request_ms"], -item["requests_per_second"]))
    else:
        recommended = max(pool, key=lambda item: (item["requests_per_second"], -item["p95_request_ms"]))
    return {
        "dataset_id": dataset_id,
        "profile_id": profile["id"],
        "objective": objective,
        "warmup_records": len(warmup),
        "sampled_records": len(sample),
        "candidates": candidates,
        "recommended": recommended,
        "met_latency_target": met_latency_target,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9090)
    parser.add_argument("--dataset", default="all")
    parser.add_argument("--profile", default="all")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--device", choices=("cpu", "gpu", "npu"), default="cpu", help="execution preference passed through BRK2")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--dry-run", action="store_true", help="validate dataset/profile and show candidate workload without network I/O")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 0.1 <= args.timeout <= 120.0:
        parser.error("port must be 1..65535 and timeout must be 0.1..120 seconds")

    manifest = read_manifest(args.manifest)
    base = args.manifest.parent
    datasets = [
        item for item in manifest["datasets"]
        if args.dataset == "all" or item.get("id") == args.dataset
    ]
    profiles = [
        item for item in manifest["profiles"]
        if (args.profile == "all" or item.get("id") == args.profile)
        and item.get("objective") in ("latency", "throughput")
    ]
    if not datasets:
        raise ValueError(f"unknown dataset {args.dataset!r}")
    if not profiles:
        raise ValueError(f"no latency/throughput profile matched {args.profile!r}")

    results = []
    for dataset in datasets:
        source_path, rows = load_dataset(base, dataset)
        for profile in profiles:
            applicable = profile.get("applicable_domains")
            if applicable is not None and dataset.get("domain") not in applicable:
                continue
            result = run_profile(
                args.host, args.port, dataset["id"], rows, profile, args.timeout, args.dry_run, args.device
            )
            if result is not None:
                result["dataset_sha256"] = sha256_file(source_path)
                results.append(result)
    if not results:
        raise ValueError("no selected profile applies to the selected datasets")

    report = {
        "manifest_version": manifest["version"],
        "manifest_sha256": sha256_file(args.manifest),
        "broker": {"host": args.host, "port": args.port, "protocol": "BRK2", "device_preference": args.device},
        "telemetry_contract": "BRK2 response fields 1..7; status, backend and selected_gpu are validated per request",
        "measured": not args.dry_run,
        "results": results,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)


if __name__ == "__main__":
    main()
