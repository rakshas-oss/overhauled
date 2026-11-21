# SPDX-License-Identifier: MIT
import importlib.util
import pathlib
import struct
import unittest

MODULE_PATH = pathlib.Path(__file__).parents[1] / "benchmarks" / "worker_fabric_saturation.py"
SPEC = importlib.util.spec_from_file_location("worker_fabric_saturation", MODULE_PATH)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


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


if __name__ == "__main__":
    unittest.main()
