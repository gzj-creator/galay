#!/usr/bin/env python3
"""Measure already-built kernel variants serially, keeping raw evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
BENCHMARKS = {
    "tcp": "tcp_socket_fair_throughput",
    "accept": "accept_completion_lifecycle",
    "resume": "scheduler_resume_pressure",
    "frame": "coroutine_frame_allocator_pressure",
}
FRAME_SAMPLES = ("frame_size_128", "frame_size_4096", "frame_mixed_recycler",
                 "task_void_complete_destroy", "task_int_complete_destroy", "task_suspend_resume")


def fields(line: str) -> dict[str, str]:
    return dict(token.split("=", 1) for token in line.replace(",", " ").split() if "=" in token)


def positive(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise ValueError(f"invalid measured value: {value}")
    return number


def parse_metrics(scenario: str, output: str) -> dict[str, float]:
    lines = output.splitlines()

    def one(prefix: str) -> dict[str, str]:
        matching = [line for line in lines if line.startswith(prefix + " ")]
        if len(matching) != 1:
            raise ValueError(f"expected one {prefix} record, got {len(matching)}")
        return fields(matching[0])

    def no_errors(record: dict[str, str], *keys: str) -> None:
        if any(int(record[key]) != 0 for key in keys):
            raise ValueError(f"benchmark reported errors: {record}")

    try:
        if scenario == "tcp":
            if lines.count("status=ok") != 1 or "status=fail" in lines:
                raise ValueError("TCP benchmark did not report success")
            measured, settled = one("measured"), one("settled")
            no_errors(measured, "runtime_errors", "shutdown_errors")
            no_errors(settled, "runtime_errors", "shutdown_errors")
            counts = [int(settled[k]) for k in
                      ("client_sent", "client_received", "server_received", "server_sent")]
            if counts[0] <= 0 or len(set(counts)) != 1:
                raise ValueError("TCP settled counters do not match")
            return {"tcp.client_pkt_s": positive(measured["client_pkt_s"])}
        if scenario == "accept":
            records = [fields(line) for line in lines if line.startswith("B41 ")]
            if sorted(int(r["batch"]) for r in records) != [1, 64]:
                raise ValueError("accept benchmark must report widths 1 and 64")
            result = {}
            for record in records:
                no_errors(record, "errors")
                positive(record["operations"])
                if positive(record["p99_us"]) < positive(record["p50_us"]):
                    raise ValueError("invalid accept latency percentiles")
                for metric in ("accepts_per_s", "p50_us", "p99_us"):
                    result[f"accept.{record['batch']}.{metric}"] = positive(record[metric])
            return result
        if scenario == "resume":
            result = {"resume.parallel.ns_per_resume": positive(one("ParallelSchedulerResume")["ns_per_resume"])}
            records = [fields(line) for line in lines if line.startswith("IOSchedulerResumeDrain ")]
            if sorted(int(r["batch"]) for r in records) != [1, 8, 64, 256]:
                raise ValueError("incomplete IO resume batch measurements")
            for record in records:
                result[f"resume.io.{record['batch']}.ns_per_task"] = positive(record["ns_per_task"])
            return result
        if scenario == "frame":
            result = {}
            for name in FRAME_SAMPLES:
                record = one(name)
                positive(record["iterations"])
                result[f"frame.{name}.ns_per_op"] = positive(record["ns_per_op"])
            return result
    except (KeyError, OverflowError) as error:
        raise ValueError(f"incomplete {scenario} measurement") from error
    raise ValueError(f"unknown scenario: {scenario}")


def command_output(command: list[str]) -> str:
    return subprocess.run(command, cwd=ROOT, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout


def measure(variants: dict[str, Path], runs: int, output: Path, cpus: str, cooldown: float,
            scenarios: list[str]) -> None:
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"output directory must be empty: {output}")
    binaries = {}
    for name, build in variants.items():
        binaries[name] = {}
        for scenario in scenarios:
            target = BENCHMARKS[scenario]
            binary = build / "benchmark/cpp/kernel" / f"benchmark_kernel_{target}"
            if not binary.is_file():
                raise ValueError(f"missing benchmark: {binary}")
            binaries[name][scenario] = binary
    output.mkdir(parents=True, exist_ok=True)
    metadata = {
        "commit": command_output(["git", "rev-parse", "HEAD"]).strip(),
        "worktree_status": command_output(["git", "status", "--short"]),
        "kernel": command_output(["uname", "-a"]).strip(),
        "cpu": command_output(["lscpu"]),
        "compiler": command_output(["c++", "--version"]),
        "affinity": cpus, "runs": runs,
        "scenarios": scenarios,
        "cooldown_seconds_before_each_repetition": cooldown,
        "ordering": "serial; reverse variant order on alternate repetitions",
        "variants": {name: str(path) for name, path in variants.items()},
        "binary_sha256": {name: {scenario: hashlib.sha256(path.read_bytes()).hexdigest()
                                 for scenario, path in paths.items()}
                          for name, paths in binaries.items()},
        "project_library_sha256": {
            name: {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in sorted((build / "src/cpp").glob("*/libgalay-*")) if path.is_file()}
            for name, build in variants.items()},
        "measurement_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    (output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (output / "source-changes.patch").write_text(command_output(
        ["git", "diff", "HEAD", "--", "src/cpp", "benchmark/cpp/kernel", "test/cpp/kernel"]))
    for name, build in variants.items():
        (output / f"{name}-CMakeCache.txt").write_bytes((build / "CMakeCache.txt").read_bytes())
    rows = []
    for repetition in range(1, runs + 1):
        if cooldown:
            print(f"cooldown {cooldown}s before repetition {repetition}", flush=True)
            time.sleep(cooldown)
        names = list(variants)
        if repetition % 2 == 0:
            names.reverse()
        for scenario in scenarios:
            for name in names:
                stem = f"{name}-{repetition}-{scenario}"
                resource_file = output / f"{stem}-resources.txt"
                command = ["/usr/bin/time", "-f", "user_s=%U system_s=%S max_rss_kb=%M elapsed_s=%e",
                           "-o", str(resource_file), "taskset", "-c", cpus,
                           str(binaries[name][scenario])]
                print(f"run {repetition}/{runs}: {name} {scenario}", flush=True)
                with (output / f"{stem}.txt").open("w") as raw:
                    result = subprocess.run(command, cwd=ROOT, stdout=raw,
                                            stderr=subprocess.STDOUT, timeout=90)
                if result.returncode != 0:
                    raise RuntimeError(f"{stem} failed with exit {result.returncode}; see raw output")
                metrics = parse_metrics(scenario, (output / f"{stem}.txt").read_text())
                resources = fields(resource_file.read_text())
                metrics.update({f"{scenario}.process.{key}": float(resources[key])
                                for key in ("user_s", "system_s", "max_rss_kb", "elapsed_s")})
                rows.extend({"variant": name, "run": repetition, "metric": key, "value": value}
                            for key, value in metrics.items())
                with (output / "samples.csv").open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=("variant", "run", "metric", "value"))
                    writer.writeheader()
                    writer.writerows(rows)
    with (output / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=("variant", "metric", "samples", "median", "min", "max"))
        writer.writeheader()
        for name, metric in sorted({(r["variant"], r["metric"]) for r in rows}):
            values = [r["value"] for r in rows if r["variant"] == name and r["metric"] == metric]
            writer.writerow(dict(variant=name, metric=metric, samples=len(values),
                                 median=statistics.median(values), min=min(values), max=max(values)))
    print(f"complete: {output / 'summary.csv'}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", action="append", required=True, help="NAME=BUILD_DIRECTORY")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--scenario", action="append", choices=tuple(BENCHMARKS),
                        help="explicit subset; omitted means all four scenarios")
    parser.add_argument("--cooldown-seconds", type=float, default=0,
                        help="fixed pause before each repetition, outside timed samples")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpus", default=",".join(map(str, sorted(os.sched_getaffinity(0)))))
    args = parser.parse_args()
    variants = {}
    for spec in args.variant:
        name, separator, path = spec.partition("=")
        if not separator or not name or not all(c.isalnum() or c in "_-" for c in name) or name in variants:
            parser.error("variants need unique alphanumeric names and a build path")
        variants[name] = Path(path).resolve()
    if args.runs < 1:
        parser.error("--runs must be positive")
    if not math.isfinite(args.cooldown_seconds) or args.cooldown_seconds < 0:
        parser.error("--cooldown-seconds must be finite and nonnegative")
    scenarios = args.scenario or list(BENCHMARKS)
    if len(scenarios) != len(set(scenarios)):
        parser.error("--scenario entries must be unique")
    measure(variants, args.runs, args.output.resolve(), args.cpus, args.cooldown_seconds, scenarios)


if __name__ == "__main__":
    main()
