#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pipelined mixed-class BRK2 saturation benchmark with per-request timing."""
import argparse
import concurrent.futures
import json
import math
import os
import pathlib
import random
import socket
import statistics
import struct
import time

MAGIC = b"BRK2"
HEADER = struct.Struct("!4sBBHHHQIHHI")
CLASSES = ("control", "realtime-spatial", "throughput", "exact-cpu", "inference")
CLASS_PATTERN = (
    "control",
    "realtime-spatial", "realtime-spatial", "realtime-spatial", "realtime-spatial",
    "throughput", "throughput", "throughput", "throughput", "throughput",
    "throughput", "throughput", "throughput", "throughput", "throughput",
    "exact-cpu",
    "inference", "inference", "inference", "inference",
)


def tlv(field_id, wire_type, flags, payload):
    return struct.pack("!HBBI", field_id, wire_type, flags, len(payload)) + payload


def class_for(sequence):
    return CLASS_PATTERN[sequence % len(CLASS_PATTERN)]


def request_frame(request_id, sequence, feature_count, worker_class, affinity_key):
    values = [((sequence * (index + 7) + index * 13) % 10007) / 10007.0
              for index in range(feature_count)]
    payload = struct.pack("!QQ" + "d" * feature_count,
                          sequence, 1_700_000_000_000_000_000 + sequence, *values)
    fields = [
        tlv(1, 8, 1, b"worker-fabric-saturation"),
        tlv(2, 8, 1, b"mixed-class"),
        tlv(3, 8, 1, b"overhauled"),
        tlv(4, 8, 1, b"inference"),
        tlv(5, 1, 1, bytes([5])),
        tlv(6, 3, 1, struct.pack("!I", 30000)),
        tlv(7, 9, 1, payload),
        tlv(10, 8, 0, b"cpu"),
        tlv(11, 8, 0, worker_class.encode("ascii")),
    ]
    if worker_class == "realtime-spatial":
        fields.append(tlv(12, 4, 0, struct.pack("!Q", affinity_key)))
    body = b"".join(fields)
    frame = HEADER.pack(MAGIC, 2, 0, 32, 3, 0, request_id, 1, 1, 0, len(body)) + body
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
    if size < HEADER.size or size > 16 * 1024 * 1024:
        raise ValueError("response frame outside size bounds")
    return recv_exact(sock, size)


def decode_response(frame, expected_id, expected_class, result_bytes):
    if len(frame) < HEADER.size:
        raise ValueError("truncated response header")
    magic, major, minor, hlen, msg_type, flags, rid, schema, version, reserved, plen = HEADER.unpack_from(frame)
    if (magic != MAGIC or major != 2 or minor != 0 or hlen != HEADER.size
            or msg_type != 4 or not flags & 1 or rid != expected_id
            or schema != 2 or version != 1 or reserved != 0
            or plen != len(frame) - HEADER.size):
        raise ValueError("invalid response envelope or correlation")
    fields = {}
    offset = HEADER.size
    while offset < len(frame):
        if len(frame) - offset < 8:
            raise ValueError("truncated response TLV")
        fid, kind, fflags, length = struct.unpack_from("!HBBI", frame, offset)
        offset += 8
        if fflags & ~3 or kind < 1 or kind > 12 or length > len(frame) - offset or fid in fields:
            raise ValueError("invalid response TLV")
        fields[fid] = (kind, frame[offset:offset + length])
        offset += length

    def req(fid, kind, size=None):
        item = fields.get(fid)
        if item is None or item[0] != kind or (size is not None and len(item[1]) != size):
            raise ValueError(f"missing or invalid response field {fid}")
        return item[1]

    task_id = req(1, 8).decode("utf-8")
    status = req(2, 1, 1)[0]
    result = req(5, 9)
    error = req(6, 8).decode("utf-8")
    backend = req(7, 8).decode("utf-8")
    actual_class = req(8, 8).decode("utf-8")
    queue_wait = struct.unpack("!Q", req(9, 4, 8))[0]
    service_time = struct.unpack("!Q", req(10, 4, 8))[0]
    worker_id = struct.unpack("!I", req(11, 3, 4))[0]
    if task_id != "worker-fabric-saturation" or status != 0:
        raise RuntimeError(f"broker rejected task: status={status}, error={error}")
    if backend != "cpu" or actual_class != expected_class or len(result) != result_bytes:
        raise ValueError("backend, worker class, or result payload mismatch")
    numa_node = None
    if 12 in fields:
        if fields[12][0] != 3 or len(fields[12][1]) != 4:
            raise ValueError("invalid NUMA telemetry")
        numa_node = struct.unpack("!I", fields[12][1])[0]
    return actual_class, queue_wait, service_time, worker_id, numa_node


def percentile(values, p):
    if not values:
        return None
    ordered = sorted(values)
    # Nearest-rank definition: rank=ceil(p*n), with zero-based indexing.
    return ordered[max(0, min(len(ordered) - 1, math.ceil(p * len(ordered)) - 1))]


def latency_ms(sent_ns, completed_ns):
    if completed_ns < sent_ns:
        raise ValueError("completion timestamp precedes send timestamp")
    return (completed_ns - sent_ns) / 1_000_000.0


def empty_stats():
    return {name: {"latency": [], "queue": [], "service": [], "worker_ids": set(),
                   "numa_nodes": set(), "requests": 0} for name in CLASSES}


def run_client(host, port, client_id, requests, depth, features, timeout):
    stats = empty_stats()
    affinity = ((client_id + 1) * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        for batch_start in range(0, requests, depth):
            count = min(depth, requests - batch_start)
            expected = {}
            # Write each frame separately but do not read responses until the
            # whole batch is in flight. This preserves pipelining while giving
            # every request its own send timestamp and correlation record.
            for index in range(count):
                local_seq = batch_start + index
                sequence = client_id * requests + local_seq
                worker_class = class_for(sequence)
                request_id = ((client_id + 1) << 48) | (local_seq + 1)
                frame = request_frame(request_id, sequence, features, worker_class, affinity)
                sent_ns = time.perf_counter_ns()
                sock.sendall(frame)
                expected[request_id] = (worker_class, sent_ns)

            for _ in range(count):
                response = recv_response(sock)
                response_id = struct.unpack_from("!Q", response, 12)[0]
                request = expected.pop(response_id, None)
                if request is None:
                    raise ValueError("unexpected or duplicate response ID")
                worker_class, sent_ns = request
                completed_ns = time.perf_counter_ns()
                actual, queue, service, worker_id, node = decode_response(
                    response, response_id, worker_class, 8 * (2 + features))
                item = stats[actual]
                item["requests"] += 1
                item["latency"].append(latency_ms(sent_ns, completed_ns))
                item["queue"].append(queue)
                item["service"].append(service)
                item["worker_ids"].add(worker_id)
                if node is not None:
                    item["numa_nodes"].add(node)
            if expected:
                raise ValueError("missing pipeline responses")
    return stats


def run_candidate(host, port, clients, requests, depth, features, timeout):
    started = time.perf_counter()
    merged = empty_stats()
    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
        futures = [pool.submit(run_client, host, port, cid, requests, depth, features, timeout)
                   for cid in range(clients)]
        for future in concurrent.futures.as_completed(futures):
            data = future.result()
            for name in CLASSES:
                dst, src = merged[name], data[name]
                dst["requests"] += src["requests"]
                dst["latency"].extend(src["latency"])
                dst["queue"].extend(src["queue"])
                dst["service"].extend(src["service"])
                dst["worker_ids"].update(src["worker_ids"])
                dst["numa_nodes"].update(src["numa_nodes"])
    elapsed = time.perf_counter() - started
    total = clients * requests
    per_class = {}
    all_latency, all_queue, all_service = [], [], []
    for name, item in merged.items():
        all_latency.extend(item["latency"])
        all_queue.extend(item["queue"])
        all_service.extend(item["service"])
        per_class[name] = {
            "requests": item["requests"],
            "requests_per_second": item["requests"] / elapsed,
            "p50_latency_ms": percentile(item["latency"], 0.50),
            "p95_latency_ms": percentile(item["latency"], 0.95),
            "p99_latency_ms": percentile(item["latency"], 0.99),
            "p95_queue_wait_us": percentile(item["queue"], 0.95),
            "mean_queue_wait_us": statistics.fmean(item["queue"]) if item["queue"] else None,
            "mean_service_time_us": statistics.fmean(item["service"]) if item["service"] else None,
            "worker_ids": sorted(item["worker_ids"]),
            "elastic_worker_ids": sorted(x for x in item["worker_ids"] if x >= 81920),
            "numa_nodes": sorted(item["numa_nodes"]),
        }
    return {
        "clients": clients, "pipeline_depth": depth, "requests_per_client": requests,
        "feature_count": features, "requests": total, "elapsed_seconds": elapsed,
        "requests_per_second": total / elapsed,
        "p50_latency_ms": percentile(all_latency, 0.50),
        "p95_latency_ms": percentile(all_latency, 0.95),
        "p99_latency_ms": percentile(all_latency, 0.99),
        "p95_queue_wait_us": percentile(all_queue, 0.95),
        "mean_queue_wait_us": statistics.fmean(all_queue),
        "mean_service_time_us": statistics.fmean(all_service),
        "per_class": per_class,
    }


def topology():
    root = pathlib.Path("/sys/devices/system/node")
    allowed = set(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else set()
    nodes = []
    if root.exists():
        for path in sorted(root.glob("node[0-9]*")):
            try:
                cpus = set()
                for part in (path / "cpulist").read_text().strip().split(","):
                    ends = [int(x) for x in part.split("-")]
                    cpus.update(range(ends[0], ends[-1] + 1))
                visible = sorted(cpus & allowed) if allowed else sorted(cpus)
                if visible:
                    nodes.append({"id": int(path.name[4:]), "visible_cpus": visible})
            except (OSError, ValueError):
                continue
    return {
        "allowed_cpu_count": len(allowed) if allowed else os.cpu_count(),
        "numa_node_ids": [node["id"] for node in nodes],
        "numa_node_count": len(nodes),
        "nodes": nodes,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9090)
    parser.add_argument("--client-counts", nargs="+", type=int, default=[8, 32])
    parser.add_argument("--pipeline-depths", nargs="+", type=int, default=[1, 32])
    parser.add_argument("--requests-per-client", type=int, default=256)
    parser.add_argument("--warmup-requests-per-client", type=int, default=64)
    parser.add_argument("--features", type=int, default=1024)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 0.1 <= args.timeout <= 120:
        parser.error("invalid port or timeout")
    if not args.client_counts or any(x < 1 or x > 128 for x in args.client_counts):
        parser.error("client counts must be in 1..128")
    if not args.pipeline_depths or any(x < 1 or x > 32 for x in args.pipeline_depths):
        parser.error("pipeline depths must be in 1..32")
    if not 1 <= args.requests_per_client <= 100000:
        parser.error("requests-per-client must be in 1..100000")
    if not 0 <= args.warmup_requests_per_client <= 100000:
        parser.error("warmup-requests-per-client must be in 0..100000")
    if not 1 <= args.features <= 60000:
        parser.error("features must be in 1..60000")

    candidates = [(c, d) for c in args.client_counts for d in args.pipeline_depths]
    random.Random(args.seed).shuffle(candidates)
    results = []
    for clients, depth in candidates:
        if args.warmup_requests_per_client:
            run_candidate(args.host, args.port, clients, args.warmup_requests_per_client,
                          depth, args.features, args.timeout)
        results.append(run_candidate(args.host, args.port, clients, args.requests_per_client,
                                     depth, args.features, args.timeout))
    report = {
        "measured": True,
        "broker": {"host": args.host, "port": args.port, "protocol": "BRK2"},
        "workload": {
            "class_mix_per_20": {"control": 1, "realtime-spatial": 4, "throughput": 10,
                                 "exact-cpu": 1, "inference": 4},
            "requests_per_client": args.requests_per_client,
            "warmup_requests_per_client": args.warmup_requests_per_client,
            "feature_count": args.features,
            "client_counts": args.client_counts,
            "pipeline_depths": args.pipeline_depths,
            "seed": args.seed,
            "latency_method": "per-request send timestamp to correlated response receipt",
        },
        "topology": topology(),
        "results": results,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)


if __name__ == "__main__":
    main()
