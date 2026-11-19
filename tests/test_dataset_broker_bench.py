#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unit tests for the BRK2 dataset workload adapter (no broker required)."""
import importlib.util
import pathlib
import socket
import struct
import tempfile
import threading
import unittest

MODULE_PATH = pathlib.Path(__file__).resolve().parents[1] / "benchmarks" / "dataset_broker_bench.py"
SPEC = importlib.util.spec_from_file_location("dataset_broker_bench", MODULE_PATH)
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)


def recv_exact(connection, size):
    result = bytearray()
    while len(result) < size:
        part = connection.recv(size - len(result))
        if not part:
            raise ConnectionError("unexpected EOF in fake broker")
        result.extend(part)
    return bytes(result)


class DatasetBrokerBenchmarkTests(unittest.TestCase):
    def test_brg2_request_contains_stable_header_and_framed_payload(self):
        record = (17, 123456789, [1.25, -2.5])
        encoded = bench.request_frame(99, "fixture", record, "cpu")
        frame_size = struct.unpack_from("!I", encoded, 0)[0]
        frame = encoded[4:]
        self.assertEqual(frame_size, len(frame))
        self.assertEqual(frame[:4], bench.MAGIC)
        self.assertEqual(struct.unpack_from("!Q", frame, 12)[0], 99)
        self.assertEqual(bench.HEADER.size, 32)

    def test_manifest_dataset_and_profile_drive_dry_run(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / "rows.jsonl").write_text(
                '{"seq":1,"ts":10,"x":1.0,"y":2.0}\n'
                '{"seq":2,"ts":20,"x":2.0,"y":3.0}\n'
                '{"seq":3,"ts":30,"x":3.0,"y":4.0}\n',
                encoding="utf-8",
            )
            spec = {
                "id": "fixture",
                "source": "rows.jsonl",
                "sequence_field": "seq",
                "timestamp_ns_field": "ts",
                "feature_fields": ["x", "y"],
            }
            _, rows = bench.load_dataset(root, spec)
            warmup, sample = bench.select_sample(rows, {
                "warmup_records": 1, "sample_records": 2,
            })
            result = bench.run_profile(
                "127.0.0.1", 9090, "fixture", rows,
                {
                    "id": "latency",
                    "objective": "latency",
                    "warmup_records": 1,
                    "sample_records": 2,
                    "batch_sizes": [1, 2],
                    "worker_counts": [1, 2],
                },
                1.0, True, "cpu",
            )
            self.assertEqual(len(warmup), 1)
            self.assertEqual(len(sample), 2)
            self.assertEqual(result["sampled_records"], 2)
            self.assertEqual(len(result["candidates"]), 4)
            self.assertEqual(result["status"], "dry_run_not_measured")

    def test_live_candidate_measures_mock_broker_round_trip(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        errors = []

        def serve():
            try:
                connection, _ = listener.accept()
                with connection:
                    for _ in range(2):
                        size = struct.unpack("!I", recv_exact(connection, 4))[0]
                        frame = recv_exact(connection, size)
                        request_id = struct.unpack_from("!Q", frame, 12)[0]
                        payload = b"".join((
                            bench.tlv(1, 8, 0, b"dataset-benchmark"),
                            bench.tlv(2, 1, 0, b"\\x00"),
                            bench.tlv(3, 5, 0, struct.pack("!q", -1)),
                            bench.tlv(4, 4, 0, struct.pack("!Q", 1)),
                            bench.tlv(5, 9, 0, struct.pack("!d", 2.0)),
                            bench.tlv(6, 8, 0, b""),
                            bench.tlv(7, 8, 0, b"cpu"),
                        ))
                        response = bench.HEADER.pack(
                            bench.MAGIC, 2, 0, 32, 4, 5, request_id, 2, 1, 0, len(payload)
                        ) + payload
                        connection.sendall(struct.pack("!I", len(response)) + response)
            except Exception as error:  # surfaced in the test thread
                errors.append(error)
            finally:
                listener.close()

        server = threading.Thread(target=serve, daemon=True)
        server.start()
        metrics = bench.run_candidate(
            "127.0.0.1", port, "fixture",
            [(1, 10, [1.0]), (2, 20, [2.0])],
            batch_size=2, workers=1, timeout=2.0, device="cpu",
        )
        server.join(timeout=3.0)
        self.assertFalse(server.is_alive())
        self.assertFalse(errors, errors)
        self.assertEqual(metrics["requests"], 2)
        self.assertGreater(metrics["requests_per_second"], 0.0)
        self.assertGreaterEqual(metrics["p95_request_ms"], 0.0)
        self.assertEqual(metrics["actual_backend_counts"], {"cpu": 2})
        self.assertEqual(metrics["fallback_requests"], 0)

    def test_response_decoder_rejects_non_ok_status_and_bad_correlation(self):
        payload = b"".join((
            bench.tlv(1, 8, 0, b"dataset-benchmark"),
            bench.tlv(2, 1, 0, b"\\x01"),
            bench.tlv(3, 5, 0, struct.pack("!q", -1)),
            bench.tlv(4, 4, 0, struct.pack("!Q", 1)),
            bench.tlv(5, 9, 0, b""),
            bench.tlv(6, 8, 0, b"rejected"),
            bench.tlv(7, 8, 0, b"cpu"),
        ))
        frame = bench.HEADER.pack(bench.MAGIC, 2, 0, 32, 4, 5, 7, 2, 1, 0, len(payload)) + payload
        with self.assertRaises(RuntimeError):
            bench.decode_response(frame, 7, "dataset-benchmark", "cpu", 0)
        with self.assertRaises(ValueError):
            bench.decode_response(frame, 8, "dataset-benchmark", "cpu", 0)

    def test_dataset_symlink_cannot_escape_manifest_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            outside = root.parent / (root.name + "-outside.jsonl")
            outside.write_text('{"seq":1,"ts":1,"x":1.0}\\n', encoding="utf-8")
            try:
                (root / "escape.jsonl").symlink_to(outside)
                spec = {
                    "id": "escape", "source": "escape.jsonl",
                    "sequence_field": "seq", "timestamp_ns_field": "ts",
                    "feature_fields": ["x"],
                }
                with self.assertRaises(ValueError):
                    bench.load_dataset(root, spec)
            finally:
                outside.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
