#!/usr/bin/env python3
"""Reject incomplete or failed performance samples before comparing variants."""

import importlib.util
import json
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
    def tcp(self, **changes):
        fields = dict(client_sent=100, client_received=100, server_received=100,
                      server_sent=100, runtime_errors=0, shutdown_errors=0)
        fields.update(changes)
        settled = "settled " + " ".join(f"{k}={v}" for k, v in fields.items())
        return "measured client_pkt_s=20 runtime_errors=0 shutdown_errors=0\n" + settled + "\nstatus=ok\n"

    def test_valid_tcp_and_settled_accounting(self):
        self.assertEqual(measure.parse_metrics("tcp", self.tcp()), {"tcp.client_pkt_s": 20.0})
        for changes in ({"client_received": 99}, {"runtime_errors": 1}, {"client_sent": 0}):
            with self.assertRaises(ValueError):
                measure.parse_metrics("tcp", self.tcp(**changes))

    def test_missing_or_failed_status_is_rejected(self):
        for output in ("", self.tcp().replace("status=ok", "status=fail"),
                       self.tcp().replace("settled ", "incomplete ")):
            with self.assertRaises(ValueError):
                measure.parse_metrics("tcp", output)

    def test_accept_requires_both_widths_and_valid_percentiles(self):
        line = "B41 batch={batch} operations=128 accepts_per_s=1000 p50_us=2 p99_us=5 errors=0\n"
        output = line.format(batch=1) + line.format(batch=64)
        self.assertEqual(measure.parse_metrics("accept", output)["accept.64.p99_us"], 5)
        for bad in (line.format(batch=1), output.replace("errors=0", "errors=1"),
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

    def test_frame_rejects_truncated_output(self):
        with self.assertRaises(ValueError):
            measure.parse_metrics("frame", "frame_size_128 iterations=1 ns_per_op=3\n")

    def test_subset_needs_only_selected_binary_and_records_cooldown(self):
        output = "ParallelSchedulerResume iterations=10000, ns_per_resume=25.5\n"
        output += "".join(f"IOSchedulerResumeDrain batch={n}, ns_per_task=12.0\n"
                          for n in (1, 8, 64, 256))

        def run(command, **kwargs):
            kwargs["stdout"].write(output)
            resource = Path(command[command.index("-o") + 1])
            resource.write_text("user_s=0.1 system_s=0.0 max_rss_kb=10 elapsed_s=0.1\n")
            return SimpleNamespace(returncode=0)

        with TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            binary = build / "benchmark/cpp/kernel/benchmark_kernel_scheduler_resume_pressure"
            binary.parent.mkdir(parents=True)
            binary.write_text("fixture: not executed")
            (build / "CMakeCache.txt").write_text("fixture\n")
            results = Path(directory) / "results"
            with patch.object(measure, "command_output", return_value="fixture\n"), \
                 patch.object(measure.subprocess, "run", side_effect=run), \
                 patch.object(measure.time, "sleep") as sleep:
                measure.measure({"fixture": build}, 1, results, "0", 2.5, ["resume"])
            sleep.assert_called_once_with(2.5)
            metadata = json.loads((results / "environment.json").read_text())
            self.assertEqual(metadata["scenarios"], ["resume"])
            self.assertEqual(metadata["cooldown_seconds_before_each_repetition"], 2.5)
            self.assertIn("resume.io.256.ns_per_task", (results / "summary.csv").read_text())
            self.assertFalse((results / "fixture-1-accept.txt").exists())


if __name__ == "__main__":
    unittest.main()
