#!/usr/bin/env python3
"""Adversarial regression harness for the Overhauled BRK2 TCP broker.

Run against an isolated test broker, e.g.:
  python3 benchmarks/adversarial_heuristics_bench.py --host 127.0.0.1 --port 9090
The harness sends real protocol frames and reports observations; it does not
infer GPU placement from latency alone. Run only against systems you own.
"""
import argparse
import concurrent.futures
import socket
import statistics
import struct
import threading
import time

MAGIC = b"BRK2"
HEADER = struct.Struct("!4sBBHHHQIHHI")  # fixed 32-byte BRK2 header


def tlv(fid, kind, flags, payload):
    return struct.pack("!HBBI", fid, kind, flags, len(payload)) + payload


def request_frame(request_id, source, payload, extra=b"", device="gpu"):
    body = b"".join((
        tlv(1, 8, 1, b"adv-bench"),
        tlv(2, 8, 1, source.encode()),
        tlv(3, 8, 1, b"overhauled"),
        tlv(4, 8, 1, b"inference"),
        tlv(5, 1, 1, b"\x05"),
        tlv(6, 3, 1, struct.pack("!I", 10000)),
        tlv(7, 9, 1, payload),
    ))
    if device:
        body += tlv(10, 8, 0, device.encode())
    body += extra
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
    if size < 32 or size > 16 * 1024 * 1024:
        raise ValueError("response frame length outside benchmark safety bounds")
    return recv_exact(sock, size)


def stats(samples):
    if not samples:
        return "n=0"
    values = sorted(samples)
    def pct(p):
        return values[min(len(values) - 1, int((len(values) - 1) * p))]
    return (f"n={len(values)} mean={statistics.mean(values):.2f}ms "
            f"p50={pct(.50):.2f}ms p95={pct(.95):.2f}ms "
            f"p99={pct(.99):.2f}ms max={values[-1]:.2f}ms")


def adv01(host, port, attackers, clients):
    print(f"\n[ADV-01] slow-drip sockets={attackers}; burst clients={clients}")
    stop = threading.Event()
    sockets = []
    lock = threading.Lock()

    def drip():
        s = None
        try:
            s = socket.create_connection((host, port), timeout=3)
            s.sendall(struct.pack("!I", 1024))
            with lock:
                sockets.append(s)
            while not stop.wait(4.5):
                s.sendall(b"X")
        except OSError:
            pass

    threads = [threading.Thread(target=drip, daemon=True) for _ in range(attackers)]
    for thread in threads:
        thread.start()
    time.sleep(0.5)

    payload = struct.pack("!8d", *range(8))
    def burst(cid):
        samples = []
        with socket.create_connection((host, port), timeout=10) as s:
            s.settimeout(10)
            for batch in range(4):
                wire = bytearray()
                sent = []
                for index in range(32):
                    rid = (cid << 32) | (batch * 32 + index + 1)
                    wire.extend(request_frame(rid, f"adv01-{cid}", payload))
                    sent.append(time.perf_counter())
                s.sendall(wire)
                for started in sent:
                    recv_response(s)
                    samples.append((time.perf_counter() - started) * 1000)
        return samples

    started = time.perf_counter()
    all_samples = []
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
            for result in pool.map(burst, range(clients)):
                all_samples.extend(result)
    finally:
        elapsed = time.perf_counter() - started
        stop.set()
        with lock:
            for s in sockets:
                try:
                    s.close()
                except OSError:
                    pass
    print(f"  established drip sockets: {len(sockets)}/{attackers}")
    print(f"  legitimate throughput: {len(all_samples) / max(elapsed, 1e-9):.1f} req/s")
    print(f"  request completion latency: {stats(all_samples)}")


def adv02(host, port):
    print("\n[ADV-02] asymmetric payload skew (latency observation only)")
    heavy = struct.pack("!65536d", *([1.25] * 65536))  # 512 KiB
    light = struct.pack("!8d", *([2.5] * 8))           # 64 B

    def stream(name, payload, count):
        samples = []
        with socket.create_connection((host, port), timeout=10) as s:
            s.settimeout(20)
            for i in range(count):
                frame = request_frame((hash(name) & 0x7fffffff) << 32 | i + 1,
                                      name, payload, device="gpu")
                before = time.perf_counter()
                s.sendall(frame)
                recv_response(s)
                samples.append((time.perf_counter() - before) * 1000)
        return samples

    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        heavy_a = pool.submit(stream, "heavy-a", heavy, 20)
        heavy_b = pool.submit(stream, "heavy-b", heavy, 20)
        light = pool.submit(stream, "light-probe", light, 40)
        a, b, l = heavy_a.result(), heavy_b.result(), light.result()
    print(f"  512 KiB streams: {stats(a + b)}")
    print(f"  64 B co-tenant: {stats(l)}")
    print("  Note: selected-GPU placement requires broker telemetry; latency is not proof.")


def adv03(host, port):
    print("\n[ADV-03] 4,096 TLV fields plus IEEE-754 special values")
    # Seven mandatory fields plus one optional device hint = eight; add 4,088
    # zero-length optional TLVs to reach exactly 4,096 fields.
    extras = b"".join(tlv(fid, 1, 0, b"") for fid in range(100, 4188))
    payload = struct.pack("!4d", float("nan"), float("inf"), float("-inf"), -0.0)
    frame = request_frame(0xA003, "adv03", payload, extra=extras, device="")
    encoded_frame_bytes = len(frame) - 4
    print(f"  encoded request frame: {encoded_frame_bytes} bytes; TLV count: 4,096")
    samples = []
    with socket.create_connection((host, port), timeout=10) as s:
        s.settimeout(10)
        for index in range(20):
            before = time.perf_counter()
            s.sendall(request_frame(0xA003 + index, "adv03", payload,
                                    extra=extras, device=""))
            recv_response(s)
            samples.append((time.perf_counter() - before) * 1000)
    print(f"  parser + response latency: {stats(samples)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9090)
    parser.add_argument("--attackers", type=int, default=120)
    parser.add_argument("--clients", type=int, default=8)
    args = parser.parse_args()
    if not (1 <= args.attackers <= 120 and 1 <= args.clients <= 32):
        parser.error("attackers must be 1..120 and clients must be 1..32")
    adv01(args.host, args.port, args.attackers, args.clients)
    adv02(args.host, args.port)
    adv03(args.host, args.port)


if __name__ == "__main__":
    main()
