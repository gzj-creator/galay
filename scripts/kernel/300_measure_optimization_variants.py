#!/usr/bin/env python3
"""Measure already-built kernel variants serially, keeping raw evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import random
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
                 "task_void_unsubmitted_destroy", "task_int_unsubmitted_destroy",
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
            return {"tcp.client_pkt_s": positive(measured["client_pkt_s"]),
                    "tcp.cpu_ns_per_op": positive(measured["cpu_ns_per_sent_packet"])}
        if scenario == "accept":
            records = [fields(line) for line in lines if line.startswith("B41 ")]
            if sorted(int(r["batch"]) for r in records) != [1, 64]:
                raise ValueError("accept benchmark must report widths 1 and 64")
            result = {}
            for record in records:
                no_errors(record, "errors")
                expected_operations = {"1": 2048, "64": 8192}[record["batch"]]
                if int(record["operations"]) != expected_operations:
                    raise ValueError("accept operation count does not match the fixed workload")
                if positive(record["p99_us"]) < positive(record["p50_us"]):
                    raise ValueError("invalid accept latency percentiles")
                for metric in ("accepts_per_s", "p50_us", "p99_us", "cpu_ns_per_op"):
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


def pair_order(runs: int, seed: int) -> list[tuple[int, int]]:
    if runs < 2 or runs % 2:
        raise ValueError("paired runs must be positive and even")
    order = [(0, 1), (1, 0)] * (runs // 2)
    random.Random(seed).shuffle(order)
    return order


def paired_comparison(baseline: list[float], candidate: list[float],
                      higher_is_better: bool) -> dict[str, float]:
    if len(baseline) != len(candidate) or len(baseline) < 2:
        raise ValueError("incomplete pairs")
    logs = [(1 if not higher_is_better else -1) * math.log(positive(b) / positive(a))
            for a, b in zip(baseline, candidate)]
    # Two-sided Student t interval for mean paired log cost ratio. Beyond 30
    # degrees of freedom use df=30 conservatively, not a normal approximation.
    critical = (12.706205, 4.302653, 3.182446, 2.776445, 2.570582, 2.446912,
                2.364624, 2.306004, 2.262157, 2.228139, 2.200985, 2.178813,
                2.160369, 2.144787, 2.131450, 2.119905, 2.109816, 2.100922,
                2.093024, 2.085963, 2.079614, 2.073873, 2.068658, 2.063899,
                2.059539, 2.055529, 2.051831, 2.048407, 2.045230, 2.042272)
    mean = statistics.mean(logs)
    margin = critical[min(len(logs) - 2, 29)] * statistics.stdev(logs) / math.sqrt(len(logs))
    costs = [math.expm1(x) for x in logs]
    return dict(degradation=math.expm1(mean), ci_low=math.expm1(mean - margin),
                ci_high=math.expm1(mean + margin), median=statistics.median(costs),
                min=min(costs), max=max(costs))


def calibrated_iterations(iterations: int, durations: list[float], minimum_ns: float) -> int:
    if not durations or iterations < 1:
        raise ValueError("empty calibration")
    fastest = min(positive(x) for x in durations)
    # 25% reserve is fixed before measurement, not an adaptive formal retry.
    return max(iterations, math.ceil(iterations * minimum_ns * 1.25 / fastest))


def command_output(command: list[str]) -> str:
    return subprocess.run(command, cwd=ROOT, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout


def observation() -> dict:
    def read(path):
        source = Path(path)
        return source.read_text() if source.exists() else "unavailable"
    return {"time_ns": time.time_ns(), "affinity": sorted(os.sched_getaffinity(0)),
            "loadavg": read("/proc/loadavg"), "stat": read("/proc/stat"),
            "cpu_mhz": [line.strip() for line in read("/proc/cpuinfo").splitlines()
                        if line.startswith("cpu MHz")],
            "pressure_cpu": read("/proc/pressure/cpu"),
            "pressure_memory": read("/proc/pressure/memory"),
            "netns": os.readlink("/proc/self/ns/net"),
            "tcp_max_tw_buckets": read("/proc/sys/net/ipv4/tcp_max_tw_buckets"),
            "conntrack_count": read("/proc/sys/net/netfilter/nf_conntrack_count"),
            "sockstat": read("/proc/net/sockstat")}


def micro_record(scenario: str, sample: str, output: str, iterations: int) -> dict[str, float]:
    prefix = sample if scenario == "frame" else (
        "ParallelSchedulerResume" if sample == "parallel" else "IOSchedulerResumeDrain")
    matches = [fields(line) for line in output.splitlines() if line.startswith(prefix + " ")]
    if len(matches) != 1:
        raise ValueError(f"missing or duplicate micro sample {sample}")
    record = matches[0]
    width = int(sample.split("_")[1]) if sample.startswith("io_") else 1
    expected = ((iterations + width - 1) // width) * width
    if int(record["iterations"]) != expected or (sample.startswith("io_") and int(record["batch"]) != width):
        raise ValueError("micro operation count differs from frozen workload")
    elapsed = positive(record["elapsed_ns"])
    cpu = positive(record["cpu_ns"])
    stem = f"{scenario}.{sample}"
    return {f"{stem}.ns_per_op": elapsed / expected,
            f"{stem}.cpu_ns_per_op": cpu / expected}


def workloads(scenarios: list[str]) -> list[tuple[str, str]]:
    return [(scenario, sample) for scenario in scenarios for sample in (
        FRAME_SAMPLES if scenario == "frame" else
        ("parallel", "io_1", "io_8", "io_64", "io_256") if scenario == "resume" else ("",))]


def write_csv(path: Path, rows: list[dict]) -> None:
    if rows:
        with path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=tuple(rows[0]))
            writer.writeheader()
            writer.writerows(rows)


def measure(variants: dict[str, Path], runs: int, output: Path, cpus: str, cooldown: float,
            scenarios: list[str], *, seed: int = 1729, minimum_ms: float = 100,
            protocol_path: Path | None = None, calibrate_only: bool = False,
            aa: bool = False, micro_cpu: str | None = None) -> None:
    if len(variants) != 2:
        raise ValueError("exactly two variant labels are required (A/A may share one build)")
    order = pair_order(runs, seed)
    if aa and runs < 10 or not aa and runs < 20:
        raise ValueError("A/A needs at least 10 pairs; A/B needs at least 20")
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"output directory must be empty: {output}")
    binaries = {name: {scenario: build / "benchmark/cpp/kernel" / f"benchmark_kernel_{BENCHMARKS[scenario]}"
                       for scenario in scenarios} for name, build in variants.items()}
    for paths in binaries.values():
        for binary in paths.values():
            if not binary.is_file():
                raise ValueError(f"missing benchmark: {binary}")
    output.mkdir(parents=True, exist_ok=True)
    names = list(variants)
    metadata = {
        "commit": command_output(["git", "rev-parse", "HEAD"]).strip(),
        "worktree_status": command_output(["git", "status", "--short"]),
        "kernel": command_output(["uname", "-a"]).strip(),
        "cpu": command_output(["lscpu"]), "compiler": command_output(["c++", "--version"]),
        "affinity": cpus, "micro_cpu": micro_cpu, "runs": runs, "scenarios": scenarios,
        "cooldown_seconds_before_each_repetition": cooldown,
        "ordering": "seeded balanced paired orders; each workload adjacent A/B",
        "seed": seed, "pair_order": [[names[i] for i in pair] for pair in order],
        "aa": aa, "minimum_ms": minimum_ms, "observation": observation(),
        "duration_scope": "B28/B34 micro segments; TCP fixed 5s and B41 fixed 2048/8192 operations",
        "ci_method": "geometric mean paired cost ratio; two-sided 95% Student t on log ratios",
        "latency_population": "B41 within-run connect-start to accept-completion per connection; CI compares run P99 values, not pooled requests",
        "variants": {name: str(path) for name, path in variants.items()},
        "binary_sha256": {name: {scenario: hashlib.sha256(path.read_bytes()).hexdigest()
                                 for scenario, path in paths.items()} for name, paths in binaries.items()},
        "project_library_sha256": {
            name: {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in sorted((build / "src/cpp").glob("*/libgalay-*")) if path.is_file()}
            for name, build in variants.items()},
        "measurement_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    if aa and (metadata["binary_sha256"][names[0]] != metadata["binary_sha256"][names[1]] or
               metadata["project_library_sha256"][names[0]] != metadata["project_library_sha256"][names[1]]):
        raise ValueError("A/A requires identical binaries AND project libraries")
    (output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (output / "source-changes.patch").write_text(command_output(
        ["git", "diff", "HEAD", "--", "src/cpp", "benchmark/cpp", "test/cpp/kernel", "scripts/kernel", "test/scripts"]))
    added_sources = command_output(["git", "ls-files", "--others", "--exclude-standard", "--",
                                    "src/cpp", "benchmark/cpp", "test/cpp/kernel", "scripts/kernel", "test/scripts"]).splitlines()
    (output / "added-sources.json").write_text(json.dumps(
        {name: (ROOT / name).read_text() for name in added_sources}, indent=2) + "\n")
    for name, build in variants.items():
        (output / f"{name}-CMakeCache.txt").write_bytes((build / "CMakeCache.txt").read_bytes())

    def run(name, scenario, sample, iterations, stem):
        resource_file = output / f"{stem}-resources.txt"
        command = ["/usr/bin/time", "-f", "user_s=%U system_s=%S max_rss_kb=%M elapsed_s=%e",
                   "-o", str(resource_file), "taskset", "-c",
                   micro_cpu if micro_cpu and sample and sample != "parallel" else cpus,
                   str(binaries[name][scenario])]
        if sample:
            command += ["--sample", sample, "--iterations", str(iterations)]
        event = {"stem": stem, "command": command, "before": observation()}
        with (output / f"{stem}.txt").open("w") as raw:
            try:
                result = subprocess.run(command, cwd=ROOT, stdout=raw, stderr=subprocess.STDOUT, timeout=90)
                event["returncode"] = result.returncode
            except subprocess.TimeoutExpired:
                event["failure"] = "90s process timeout"
                raise
            finally:
                event["after"] = observation()
                with (output / "events.jsonl").open("a") as stream:
                    stream.write(json.dumps(event) + "\n")
        if result.returncode:
            raise RuntimeError(f"{stem} failed with exit {result.returncode}; see raw output")
        raw = (output / f"{stem}.txt").read_text()
        metrics = micro_record(scenario, sample, raw, iterations) if sample else parse_metrics(scenario, raw)
        resources = fields(resource_file.read_text())
        metric_prefix = f"{scenario}.{sample}" if sample else scenario
        metrics.update({f"{metric_prefix}.process.{key}": float(resources[key])
                        for key in ("user_s", "system_s", "max_rss_kb", "elapsed_s")})
        # Process totals include setup/warmup/drain and are never mislabeled CPU/op.
        return metrics, raw

    protocol = json.loads(protocol_path.read_text()) if protocol_path else {
        "version": 2, "minimum_ms": minimum_ms, "iterations": {}, "scenarios": scenarios,
        "cpus": cpus, "micro_cpu": micro_cpu}
    if (protocol["version"] != 2 or protocol["scenarios"] != scenarios or protocol["minimum_ms"] != minimum_ms
            or protocol["cpus"] != cpus or protocol["micro_cpu"] != micro_cpu):
        raise ValueError("frozen protocol does not match requested workload or duration")
    for scenario, sample in workloads(scenarios):
        if not sample:
            continue
        key = f"{scenario}.{sample}"
        if protocol_path:
            if type(protocol["iterations"].get(key)) is not int or not 1 <= protocol["iterations"][key] <= 1_000_000_000:
                raise ValueError(f"invalid frozen count: {key}")
            continue
        count = 100_000
        durations = []
        # Independent preflight; all calibration/warmup logs are preserved.
        for name in names:
            _, raw = run(name, scenario, sample, count, f"calibration-{name}-{key}")
            durations.append(float(next(fields(line)["elapsed_ns"] for line in raw.splitlines()
                                        if "elapsed_ns=" in line)))
        count = calibrated_iterations(count, durations, minimum_ms * 1e6)
        if count > 1_000_000_000:
            raise ValueError("calibration exceeds supported iteration count")
        for attempt in range(3):
            durations = []
            for name in names:
                _, raw = run(name, scenario, sample, count, f"preflight-{attempt}-{name}-{key}")
                durations.append(float(next(fields(line)["elapsed_ns"] for line in raw.splitlines()
                                            if "elapsed_ns=" in line)))
            if min(durations) >= minimum_ms * 1e6:
                break
            count = calibrated_iterations(count, durations, minimum_ms * 1e6)
        else:
            raise ValueError(f"unstable calibration: {key}; freeze a new protocol before measuring")
        protocol["iterations"][key] = count
    (output / "protocol.json").write_text(json.dumps(protocol, indent=2) + "\n")
    if calibrate_only:
        return
    rows = []
    duration_failures = []
    for repetition, pair in enumerate(order, 1):
        if cooldown:
            time.sleep(cooldown)
        print(f"pair {repetition}/{runs}: {[names[i] for i in pair]}", flush=True)
        for scenario, sample in workloads(scenarios):
            count = protocol["iterations"].get(f"{scenario}.{sample}", 0)
            for index in pair:
                name = names[index]
                stem = f"{name}-{repetition}-{scenario}" + (f"-{sample}" if sample else "")
                metrics, raw = run(name, scenario, sample, count, stem)
                if sample:
                    duration = float(next(fields(line)["elapsed_ns"] for line in raw.splitlines() if "elapsed_ns=" in line))
                    if duration < minimum_ms * 1e6:
                        duration_failures.append({"stem": stem, "elapsed_ns": duration})
                rows.extend({"variant": name, "run": repetition, "metric": key, "value": value}
                            for key, value in metrics.items())
                write_csv(output / "samples.csv", rows)
    summary, comparison = [], []
    for metric in sorted({r["metric"] for r in rows}):
        values = {}
        for name in names:
            values[name] = [r["value"] for r in rows if r["variant"] == name and r["metric"] == metric]
            summary.append(dict(variant=name, metric=metric, samples=len(values[name]),
                                median=statistics.median(values[name]), min=min(values[name]), max=max(values[name])))
        if ".process." in metric and not metric.endswith("max_rss_kb"):
            continue
        result = paired_comparison(values[names[0]], values[names[1]], metric.endswith(("pkt_s", "accepts_per_s")))
        bound = .05 if metric.endswith(("cpu_ns_per_op", "max_rss_kb")) else .03 if metric.endswith("p99_us") else .02
        precise_aa = result["ci_low"] >= -bound and result["ci_high"] <= bound
        comparison.append(dict(metric=metric, pairs=runs, **result, regression_bound=bound,
                               protection_pass=result["ci_high"] <= bound,
                               target_gain_pass=result["degradation"] <= -.02 and result["ci_high"] < 0,
                               aa_precise=precise_aa, aa_contains_zero=result["ci_low"] <= 0 <= result["ci_high"]))
    write_csv(output / "summary.csv", summary)
    write_csv(output / "paired.csv", comparison)
    (output / "gate.json").write_text(json.dumps({
        "duration_pass": not duration_failures, "short_segments": duration_failures,
        "protection_pass": all(r["protection_pass"] for r in comparison),
        "aa_precision_pass": all(r["aa_precise"] for r in comparison) if aa else None,
        "aa_bias_metrics": [r["metric"] for r in comparison if not r["aa_contains_zero"]] if aa else None,
        "adoption": "manual: additionally requires declared target, correctness, A/A precision and independent confirmation",
    }, indent=2) + "\n")
    print(f"complete: {output / 'paired.csv'}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", action="append", required=True, help="NAME=BUILD_DIRECTORY; baseline first")
    parser.add_argument("--runs", type=int, default=20, help="number of balanced pairs")
    parser.add_argument("--aa", action="store_true", help="identical artifact measurement gate; minimum 10 pairs")
    parser.add_argument("--seed", type=int, default=1729)
    parser.add_argument("--minimum-ms", type=float, default=100)
    parser.add_argument("--micro-cpu", help="pin single-thread micro workloads separately; recorded in frozen protocol")
    parser.add_argument("--protocol", type=Path, help="reuse a frozen calibration protocol")
    parser.add_argument("--calibrate-only", action="store_true")
    parser.add_argument("--scenario", action="append", choices=tuple(BENCHMARKS))
    parser.add_argument("--cooldown-seconds", type=float, default=0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpus", default=",".join(map(str, sorted(os.sched_getaffinity(0)))))
    args = parser.parse_args()
    variants = {}
    for spec in args.variant:
        name, separator, path = spec.partition("=")
        if not separator or not name or not all(c.isalnum() or c in "_-" for c in name) or name in variants:
            parser.error("variants need unique alphanumeric names and a build path")
        variants[name] = Path(path).resolve()
    if not math.isfinite(args.cooldown_seconds) or args.cooldown_seconds < 0:
        parser.error("--cooldown-seconds must be finite and nonnegative")
    if not math.isfinite(args.minimum_ms) or args.minimum_ms < 100:
        parser.error("--minimum-ms must be finite and at least 100")
    scenarios = args.scenario or list(BENCHMARKS)
    if len(scenarios) != len(set(scenarios)):
        parser.error("--scenario entries must be unique")
    measure(variants, args.runs, args.output.resolve(), args.cpus, args.cooldown_seconds, scenarios,
            seed=args.seed, minimum_ms=args.minimum_ms, protocol_path=args.protocol,
            calibrate_only=args.calibrate_only, aa=args.aa, micro_cpu=args.micro_cpu)


if __name__ == "__main__":
    main()
