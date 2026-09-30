#!/usr/bin/env python3
"""Reject incomplete or failed performance samples before comparing variants."""

import importlib.util
import json
import math
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "measure", ROOT / "scripts/kernel/300_measure_optimization_variants.py")
measure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(measure)


class MeasurementTest(unittest.TestCase):
    def test_paired_cost_ratios_and_known_log_interval(self):
        # Symmetric log ratios: geometric mean 1, exact t interval.
        a = [100.0] * 10
        b = [100 * math.exp(x) for x in [-.01, .01] * 5]
        result = measure.paired_comparison(a, b, higher_is_better=False)
        self.assertAlmostEqual(result["degradation"], 0.0)
        margin = 2.262157 * .01 / 3
        self.assertAlmostEqual(result["ci_low"], math.expm1(-margin), places=7)
        self.assertAlmostEqual(result["ci_high"], math.expm1(margin), places=7)
        for higher, candidate in [(False, 98), (True, 100 / .98)]:
            result = measure.paired_comparison(a, [candidate] * 10, higher)
            self.assertAlmostEqual(result["degradation"], -.02)
            self.assertAlmostEqual(result["ci_low"], -.02)
            self.assertAlmostEqual(result["ci_high"], -.02)
        for bad in ([100] * 9, [0] * 10, [float("nan")] * 10):
            with self.assertRaises(ValueError):
                measure.paired_comparison(a, bad, False)

    def test_balanced_seeded_pairs(self):
        order = measure.pair_order(20, 1729)
        self.assertEqual(order.count((0, 1)), 10)
        self.assertEqual(order.count((1, 0)), 10)
        self.assertEqual(order, measure.pair_order(20, 1729))
        self.assertNotEqual(order, measure.pair_order(20, 1730))
        with self.assertRaises(ValueError):
            measure.pair_order(19, 1729)

    def test_frozen_counts_use_fastest_sample(self):
        self.assertEqual(measure.calibrated_iterations(1000, [10e6, 20e6], 100e6), 12500)
        with self.assertRaises(ValueError):
            measure.calibrated_iterations(1000, [0], 100e6)

    def tcp(self, **changes):
        fields = dict(client_sent=100, client_received=100, server_received=100,
                      server_sent=100, runtime_errors=0, shutdown_errors=0)
        fields.update(changes)
        settled = "settled " + " ".join(f"{k}={v}" for k, v in fields.items())
        return "measured client_pkt_s=20 cpu_ns_per_sent_packet=50 runtime_errors=0 shutdown_errors=0\n" + settled + "\nstatus=ok\n"

    def test_valid_tcp_and_settled_accounting(self):
        self.assertEqual(measure.parse_metrics("tcp", self.tcp()), {"tcp.client_pkt_s": 20.0, "tcp.cpu_ns_per_op": 50.0})
        for changes in ({"client_received": 99}, {"runtime_errors": 1}, {"client_sent": 0}):
            with self.assertRaises(ValueError):
                measure.parse_metrics("tcp", self.tcp(**changes))

    def test_missing_or_failed_status_is_rejected(self):
        for output in ("", self.tcp().replace("status=ok", "status=fail"),
                       self.tcp().replace("settled ", "incomplete ")):
            with self.assertRaises(ValueError):
                measure.parse_metrics("tcp", output)

    def test_accept_requires_both_widths_and_valid_percentiles(self):
        line = "B41 batch={batch} operations={operations} accepts_per_s=1000 p50_us=2 p99_us=5 cpu_ns_per_op=80 errors=0\n"
        single = line.format(batch=1, operations=2048)
        output = single + line.format(batch=64, operations=8192)
        self.assertEqual(measure.parse_metrics("accept", output)["accept.64.p99_us"], 5)
        for bad in (single, output.replace("errors=0", "errors=1"),
                    output.replace("operations=2048", "operations=2047"),
                    output.replace("operations=8192", "operations=8256"),
                    output.replace("p99_us=5", "p99_us=1"),
                    output.replace("accepts_per_s=1000", "accepts_per_s=nan")):
            with self.assertRaises(ValueError):
                measure.parse_metrics("accept", bad)

    def test_resume_requires_all_batch_sizes(self):
        output = "ParallelSchedulerResume iterations=10000, ns_per_resume=25.5\n"
        output += "".join(f"IOSchedulerResumeDrain batch={n}, ns_per_task=12.0\n"
                          for n in (1, 8, 64, 256))
        self.assertEqual(len(measure.parse_metrics("resume", output)), 5)
        with self.assertRaises(ValueError):
            measure.parse_metrics("resume", output.replace("batch=256", "batch=255"))

    def test_micro_accounting_and_duration(self):
        raw = "IOSchedulerResumeDrain iterations=1024 batch=256 elapsed_ns=128000000 cpu_ns=120000000\n"
        self.assertEqual(measure.micro_record("resume", "io_256", raw, 1000)["resume.io_256.ns_per_op"], 125000)
        for bad in (raw.replace("1024", "1000"), raw.replace("256", "64"), raw + raw,
                    raw.replace("cpu_ns=120000000", "cpu_ns=-1")):
            with self.assertRaises(ValueError):
                measure.micro_record("resume", "io_256", bad, 1000)

    def test_single_io_batch_identity_is_checked(self):
        raw = "IOSchedulerResumeDrain iterations=1000 batch=8 elapsed_ns=100000000 cpu_ns=90000000\n"
        with self.assertRaises(ValueError):
            measure.micro_record("resume", "io_1", raw, 1000)

    def test_ring_samples_and_ownership_accounting(self):
        self.assertEqual(measure.workloads(["ring"]),
                         [("ring", sample) for sample in
                          ("owner-1", "owner-64", "shared-1", "shared-64")])
        raw = ("ReadyRing sample=owner-64 iterations=1000 elapsed_ns=100000000 "
               "cpu_ns=90000000 releases=1000 errors=0\n")
        result = measure.micro_record("ring", "owner-64", raw, 1000)
        self.assertEqual(result["ring.owner-64.ns_per_op"], 100000)
        for bad in (raw.replace("owner-64", "shared-64"),
                    raw.replace("iterations=1000", "iterations=1024"),
                    raw.replace("releases=1000", "releases=999"),
                    raw.replace("releases=1000", "releases=1001"),
                    raw.replace("releases=1000", ""),
                    raw.replace("cpu_ns=90000000", "cpu_ns=-1"),
                    raw.replace("errors=0", "errors=1"), raw + raw):
            with self.subTest(raw=bad), self.assertRaises(ValueError):
                measure.micro_record("ring", "owner-64", bad, 1000)

    def test_ring_is_explicit_and_default_matrix_stays_focused(self):
        for args, expected in (([], ["tcp", "accept", "resume", "frame"]),
                               (["--scenario", "ring"], ["ring"])):
            with self.subTest(args=args), patch("sys.argv", [
                    "measure", "--variant", "a=build/a", "--variant", "b=build/b",
                    "--output", "build/results", *args]), \
                    patch.object(measure, "measure") as run:
                measure.main()
                self.assertEqual(run.call_args.args[5], expected)

    def test_components_cannot_replace_parked_accept_gate(self):
        raw = "B41Components implementation=galay batch=1 operations=2048 ready_accept_ns_per_op=100 errors=0\n"
        raw += "B41Components implementation=galay batch=64 operations=8192 ready_accept_ns_per_op=100 errors=0\n"
        with self.assertRaises(ValueError):
            measure.parse_metrics("accept", raw)

    def test_frame_rejects_truncated_output(self):
        with self.assertRaises(ValueError):
            measure.parse_metrics("frame", "frame_size_128 iterations=1 ns_per_op=3\n")

    def test_subset_needs_only_selected_binary_and_records_cooldown(self):
        def run(command, **kwargs):
            sample = command[command.index("--sample") + 1]
            count = int(command[command.index("--iterations") + 1])
            width = int(sample.split("_")[1]) if sample.startswith("io_") else 1
            actual = ((count + width - 1) // width) * width
            prefix = "ParallelSchedulerResume" if sample == "parallel" else "IOSchedulerResumeDrain"
            kwargs["stdout"].write(f"{prefix} iterations={actual} batch={width} elapsed_ns=200000000 cpu_ns=150000000\n")
            resource = Path(command[command.index("-o") + 1])
            resource.write_text("user_s=0.1 system_s=0.0 max_rss_kb=10 elapsed_s=0.2\n")
            return SimpleNamespace(returncode=0)

        with TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            binary = build / "benchmark/cpp/kernel/benchmark_kernel_scheduler_resume_pressure"
            binary.parent.mkdir(parents=True)
            binary.write_text("fixture: not executed")
            (build / "CMakeCache.txt").write_text("fixture\n")
            results = Path(directory) / "results"
            with patch.object(measure, "command_output", side_effect=lambda cmd: "" if "ls-files" in cmd else "fixture\n"), \
                 patch.object(measure.subprocess, "run", side_effect=run), \
                 patch.object(measure.time, "sleep") as sleep:
                measure.measure({"a": build, "b": build}, 10, results, "0", 2.5, ["resume"], aa=True)
            self.assertEqual(sleep.call_count, 10)
            sleep.assert_called_with(2.5)
            metadata = json.loads((results / "environment.json").read_text())
            self.assertEqual(metadata["scenarios"], ["resume"])
            self.assertEqual(metadata["cooldown_seconds_before_each_repetition"], 2.5)
            self.assertIn("resume.io_256.ns_per_op", (results / "summary.csv").read_text())
            self.assertFalse((results / "a-1-accept.txt").exists())


if __name__ == "__main__":
    unittest.main()
