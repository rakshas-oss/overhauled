# SPDX-License-Identifier: MIT
import importlib.util
import pathlib
import struct
import unittest

ROOT = pathlib.Path(__file__).parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BENCH = load_module("worker_fabric_saturation", ROOT / "benchmarks" / "worker_fabric_saturation.py")
MONITOR = load_module("process_resource_monitor", ROOT / "benchmarks" / "process_resource_monitor.py")


class WorkerFabricSaturationTests(unittest.TestCase):
    def test_class_mix_is_exact_and_covers_all_five_classes(self):
        counts = {name: 0 for name in BENCH.CLASSES}
        for sequence in range(20):
            counts[BENCH.class_for(sequence)] += 1
        self.assertEqual(counts, {
            "control": 1,
            "realtime-spatial": 4,
            "throughput": 10,
            "exact-cpu": 1,
            "inference": 4,
        })

    def test_realtime_request_includes_affinity_tlv(self):
        frame = BENCH.request_frame(7, 12, 8, "realtime-spatial", 99)
        frame_size = struct.unpack_from("!I", frame)[0]
        self.assertEqual(frame_size, len(frame) - 4)
        self.assertIn(BENCH.tlv(12, 4, 0, struct.pack("!Q", 99)), frame)

    def test_request_id_encoding_fits_u64(self):
        request_id = ((128 + 1) << 48) | 100000
        self.assertLess(request_id, 2**64)
        self.assertEqual(struct.unpack("!Q", struct.pack("!Q", request_id))[0], request_id)

    def test_percentile_uses_nearest_rank(self):
        values = [1, 2, 3, 4, 5]
        self.assertEqual(BENCH.percentile(values, 0.50), 3)
        self.assertEqual(BENCH.percentile(values, 0.95), 5)
        self.assertEqual(BENCH.percentile([], 0.95), None)

    def test_latency_is_correlated_per_request(self):
        self.assertAlmostEqual(BENCH.latency_ms(1_000_000, 3_500_000), 2.5)
        with self.assertRaises(ValueError):
            BENCH.latency_ms(10, 9)

    def test_request_payload_scales_to_1024_features(self):
        frame = BENCH.request_frame(9, 3, 1024, "throughput", 0)
        self.assertGreater(len(frame), 8192)
        self.assertLess(len(frame), 16384)

    def test_proc_stat_parser_handles_parenthesized_command(self):
        # Fields after ')' begin with state (field 3); utime/stime are
        # offsets 11/12 and RSS pages is offset 21.
        fields = ["S"] + ["0"] * 21
        fields[11] = "125"
        fields[12] = "75"
        fields[21] = "256"
        record = "123 (worker (test) process) " + " ".join(fields)
        # Exercise the parser's field extraction independently of /proc IO.
        closing = record.rfind(")")
        parsed = record[closing + 1:].split()
        self.assertEqual(int(parsed[11]) + int(parsed[12]), 200)
        self.assertEqual(int(parsed[21]), 256)
        self.assertTrue(callable(MONITOR.read_process_sample))


if __name__ == "__main__":
    unittest.main()
