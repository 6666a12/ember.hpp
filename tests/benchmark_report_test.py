"""Checks statistical definitions and rejects corrupted/incomplete timing data."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "benchmark_runner", Path(__file__).resolve().parents[1] / "tools" / "run_benchmarks.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def fixture():
    return {"schema_version": 1, "workload_version": 1, "build_type": "Release",
            "variant": "plain", "gpu_timers": True, "measured_frames": 2, "repeats": 1,
            "cases": [{"scenario": "dense", "capacity": 100, "repeat": 0,
                       "preflight_changed_pixels": 10, "window_wall_ms": 6,
                       "samples": [{"frame": i, "alive": 100, "cpu_frame_ms": i + 2,
                                    "cpu_update_ms": 1, "cpu_render_ms": .5,
                                    "gpu_frame_span_ms": i + 1} for i in range(2)]}]}


class ReportTest(unittest.TestCase):
    def test_nearest_rank(self):
        result = runner.stats(list(range(1, 101)))
        self.assertEqual((result["p50"], result["p95"], result["p99"]), (50, 95, 99))
        self.assertEqual(result["mean"], 50.5)
        self.assertEqual(runner.stats([7])["p99"], 7)

    def test_aggregate(self):
        c = runner.aggregate(fixture())["cases"][0]
        self.assertEqual(c["metrics"]["cpu_frame_ms"]["p50"], 2)
        self.assertEqual(c["completed_wall_ms_per_frame"]["mean"], 3)

    def test_deferred_population_capture(self):
        raw = fixture()
        raw.update(measurement_version=2, scheduling="gpu", population_capture="gpu_history_after_window")
        runner.validate(raw)
        raw["population_capture"] = "immediate"
        with self.assertRaises(ValueError):
            runner.validate(raw)

    def test_reject_corruption(self):
        changes = [
            lambda r: r["cases"][0]["samples"].pop(),
            lambda r: r["cases"][0]["samples"][0].update(cpu_frame_ms=float("nan")),
            lambda r: r["cases"][0]["samples"][0].update(gpu_frame_span_ms=0),
            lambda r: r["cases"][0]["samples"][0].update(alive=101),
            lambda r: r.update(variant="stages"),  # absent instrumentation
            lambda r: r.update(repeats=2),  # missing run
            lambda r: r["cases"].append(copy.deepcopy(r["cases"][0])),
            lambda r: r.update(build_type="Debug"),
            lambda r: r.update(scheduling="gpu"),
            lambda r: r.update(measurement_version=3),
        ]
        for change in changes:
            raw = fixture()
            change(raw)
            with self.subTest(change=change), self.assertRaises(ValueError):
                runner.aggregate(raw)


if __name__ == "__main__":
    unittest.main()
