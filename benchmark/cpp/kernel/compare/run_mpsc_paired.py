#!/usr/bin/env python3
"""Run paired C++/Rust MPSC benchmarks with one shared workload."""

from __future__ import annotations

import argparse
import csv
import fcntl
import hashlib
import json
import math
import os
import platform
import random
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


LANGUAGES = ("cpp", "rust")
SUPPORTED_PLACEMENTS = {"pinned", "perf-class-only"}
REQUIRED_FIELDS = {
    "schema",
    "language",
    "case",
    "topology",
    "ordering",
    "payload_bytes",
    "capacity",
    "messages",
    "elapsed_ns",
    "messages_per_second",
    "received",
    "checksum",
    "expected_checksum",
    "fifo_ok",
    "full_retries",
    "empty_retries",
    "producer_placement",
    "consumer_placement",
    "backoff",
    "generator",
    "valid",
}


def comma_separated_ints(value: str) -> list[int]:
    try:
        values = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as error:
        raise argparse.ArgumentTypeError("values must be integers") from error
    if not values:
        raise argparse.ArgumentTypeError("at least one value is required")
    return values


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Calibrate and run paired Galay C++ / Rust MPSC benchmarks."
    )
    parser.add_argument("--cpp-binary", type=Path, required=True)
    parser.add_argument("--rust-binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--cases",
        default="bounded,unbounded",
        help="comma-separated subset of bounded,unbounded",
    )
    parser.add_argument(
        "--producers",
        type=comma_separated_ints,
        default=comma_separated_ints("2,4,8"),
        help="comma-separated producer counts (default: 2,4,8)",
    )
    parser.add_argument("--capacity", type=int, default=4096)
    parser.add_argument("--initial-messages", type=int, default=1_000_000)
    parser.add_argument("--max-messages", type=int, default=2_000_000_000)
    parser.add_argument("--target-seconds", type=float, default=1.0)
    parser.add_argument("--calibration-probes", type=int, default=3)
    parser.add_argument("--calibration-attempts", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--samples", type=int, default=16)
    parser.add_argument("--bootstrap-resamples", type=int, default=10_000)
    parser.add_argument(
        "--process-timeout",
        type=float,
        default=300.0,
        help="per-run wall-clock timeout in seconds (default: 300)",
    )
    parser.add_argument("--minimum-speedup", type=float, default=1.0)
    parser.add_argument("--max-cv-percent", type=float, default=3.0)
    parser.add_argument("--producer-core", type=int, default=0)
    parser.add_argument("--consumer-core", type=int, default=8)
    args = parser.parse_args()

    allowed_cases = {"bounded", "unbounded"}
    cases = [value.strip() for value in args.cases.split(",") if value.strip()]
    if not cases or any(value not in allowed_cases for value in cases):
        parser.error("--cases must contain bounded and/or unbounded")
    if len(set(cases)) != len(cases):
        parser.error("--cases must not contain duplicates")
    if any(value < 2 or value > 32 for value in args.producers):
        parser.error("--producers values must be between 2 and 32")
    if len(set(args.producers)) != len(args.producers):
        parser.error("--producers must not contain duplicates")
    if args.capacity < 2 or args.capacity & (args.capacity - 1):
        parser.error("--capacity must be a power of two and at least 2")
    if args.initial_messages <= 0 or args.max_messages < args.initial_messages:
        parser.error("message bounds are invalid")
    if args.max_messages > 0xffff_ffff:
        parser.error("--max-messages must fit the 32-bit per-producer sequence field")
    if not math.isfinite(args.target_seconds) or args.target_seconds <= 0:
        parser.error("--target-seconds must be finite and positive")
    if args.calibration_probes <= 0 or args.calibration_attempts <= 0:
        parser.error("calibration counts must be positive")
    if args.warmups < 1 or args.samples < 16 or args.samples % 4 != 0:
        parser.error(
            "at least one warmup and a multiple of four paired samples "
            "no smaller than 16 are required"
        )
    if args.bootstrap_resamples < 1_000:
        parser.error("--bootstrap-resamples must be at least 1000")
    if (
        not math.isfinite(args.process_timeout)
        or args.process_timeout <= args.target_seconds * 1.50
    ):
        parser.error("--process-timeout must exceed 1.5 * --target-seconds")
    if (
        not math.isfinite(args.minimum_speedup)
        or args.minimum_speedup < 1.0
        or not math.isfinite(args.max_cv_percent)
        or args.max_cv_percent <= 0.0
    ):
        parser.error("acceptance thresholds are invalid")
    if args.producer_core < 0 or args.consumer_core < 0:
        parser.error("core indices must be non-negative")
    largest_producer_end = args.producer_core + max(args.producers)
    if args.producer_core <= args.consumer_core < largest_producer_end:
        parser.error("consumer core must not overlap any requested producer range")

    args.cases = cases
    return args


def binary_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as binary:
        while chunk := binary.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def workload_checksum(messages: int, producers: int) -> int:
    checksum = 0
    for producer in range(producers):
        first = messages * producer // producers
        end = messages * (producer + 1) // producers
        count = end - first
        checksum += (producer << 32) * count + count * (count - 1) // 2
    return checksum & 0xffff_ffff_ffff_ffff


def run_once(
    binary: Path,
    language: str,
    case_name: str,
    capacity: int,
    messages: int,
    producers: int,
    producer_core: int,
    consumer_core: int,
    process_timeout: float,
) -> dict[str, Any]:
    command = [
        str(binary),
        "--case",
        case_name,
        "--capacity",
        str(capacity),
        "--messages",
        str(messages),
        "--producers",
        str(producers),
        "--producer-core",
        str(producer_core),
        "--consumer-core",
        str(consumer_core),
    ]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            check=False,
            timeout=process_timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"{language} {case_name} {producers}p1c benchmark exceeded "
            f"the per-run timeout of {process_timeout:.1f}s"
        ) from error
    if completed.returncode != 0:
        raise RuntimeError(
            f"{language} benchmark failed with {completed.returncode}: "
            f"{completed.stderr.strip() or completed.stdout.strip()}"
        )
    output_lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if len(output_lines) != 1:
        raise RuntimeError(f"{language} benchmark must emit exactly one JSON line")
    try:
        result = json.loads(output_lines[0])
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{language} emitted invalid JSON: {error}") from error
    if not isinstance(result, dict):
        raise RuntimeError(f"{language} benchmark output must be a JSON object")

    missing = REQUIRED_FIELDS.difference(result)
    if missing:
        raise RuntimeError(f"{language} result misses fields: {sorted(missing)}")
    expected_capacity = capacity if case_name == "bounded" else 0
    expected = {
        "schema": "galay.mpsc.paired.v1",
        "language": language,
        "case": case_name,
        "topology": f"{producers}p1c",
        "ordering": "producer_fifo",
        "payload_bytes": 8,
        "capacity": expected_capacity,
        "messages": messages,
        "received": messages,
        "backoff": "yield",
        "generator": "per_producer_monotonic_u64",
        "valid": True,
        "fifo_ok": True,
    }
    for key, expected_value in expected.items():
        if result[key] != expected_value:
            raise RuntimeError(
                f"{language} field {key}={result[key]!r}, expected {expected_value!r}"
            )
    if result["valid"] is not True or result["fifo_ok"] is not True:
        raise RuntimeError(f"{language} validity fields must be JSON booleans")
    integer_fields = (
        "payload_bytes",
        "capacity",
        "messages",
        "elapsed_ns",
        "received",
        "checksum",
        "expected_checksum",
        "full_retries",
        "empty_retries",
    )
    for field in integer_fields:
        if isinstance(result[field], bool) or not isinstance(result[field], int):
            raise RuntimeError(f"{language} field {field} must be an integer")
        if result[field] < 0:
            raise RuntimeError(f"{language} field {field} must not be negative")
    throughput = result["messages_per_second"]
    if isinstance(throughput, bool) or not isinstance(throughput, (int, float)):
        raise RuntimeError(f"{language} messages_per_second must be numeric")
    if not math.isfinite(throughput) or throughput <= 0 or result["elapsed_ns"] <= 0:
        raise RuntimeError(f"{language} reported a non-positive measurement")
    calculated_throughput = messages * 1_000_000_000.0 / result["elapsed_ns"]
    if not math.isclose(throughput, calculated_throughput, rel_tol=1e-12):
        raise RuntimeError(f"{language} throughput does not match elapsed time")
    expected_checksum = workload_checksum(messages, producers)
    if (
        result["checksum"] != expected_checksum
        or result["expected_checksum"] != expected_checksum
    ):
        raise RuntimeError(f"{language} checksum validation failed")
    if case_name == "unbounded" and result["full_retries"] != 0:
        raise RuntimeError(f"{language} unbounded case reported full retries")
    placements = (result["producer_placement"], result["consumer_placement"])
    if any(
        not isinstance(value, str) or value not in SUPPORTED_PLACEMENTS
        for value in placements
    ):
        raise RuntimeError(
            f"{language} returned unsupported thread placement {placements!r}"
        )
    result["command"] = command
    return result


def language_order(index: int) -> tuple[str, str]:
    # ABBA ordering balances first-run, thermal, and short-term frequency bias.
    return LANGUAGES if index % 4 in (0, 3) else tuple(reversed(LANGUAGES))


def run_pair(
    binaries: dict[str, Path],
    case_name: str,
    capacity: int,
    messages: int,
    producers: int,
    producer_core: int,
    consumer_core: int,
    order_index: int,
    process_timeout: float,
) -> list[dict[str, Any]]:
    rows = [
        run_once(
            binaries[language],
            language,
            case_name,
            capacity,
            messages,
            producers,
            producer_core,
            consumer_core,
            process_timeout,
        )
        for language in language_order(order_index)
    ]
    placements = {
        row["language"]: (
            row["producer_placement"],
            row["consumer_placement"],
        )
        for row in rows
    }
    if placements["cpp"] != placements["rust"]:
        raise RuntimeError(
            f"{case_name} {producers}p1c pair {order_index} used different "
            "C++/Rust placement"
        )
    return rows


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round(probability * (len(ordered) - 1))))
    return ordered[index]


def bootstrap_median_ci(
    values: list[float], resamples: int, seed: int
) -> tuple[float, float]:
    generator = random.Random(seed)
    medians = []
    for _ in range(resamples):
        sample = [values[generator.randrange(len(values))] for _ in values]
        medians.append(statistics.median(sample))
    return percentile(medians, 0.025), percentile(medians, 0.975)


def bootstrap_abba_block_median_ci(
    values: list[float], resamples: int, seed: int
) -> tuple[float, float]:
    blocks = [values[index : index + 4] for index in range(0, len(values), 4)]
    generator = random.Random(seed)
    medians = []
    for _ in range(resamples):
        sample = []
        for _ in blocks:
            sample.extend(blocks[generator.randrange(len(blocks))])
        medians.append(statistics.median(sample))
    return percentile(medians, 0.025), percentile(medians, 0.975)


def summarize(values: list[float], resamples: int, seed: int) -> dict[str, float]:
    mean = statistics.fmean(values)
    deviation = statistics.stdev(values)
    ci_low, ci_high = bootstrap_median_ci(values, resamples, seed)
    return {
        "median": statistics.median(values),
        "mean": mean,
        "cv_percent": deviation * 100.0 / mean,
        "median_ci95_low": ci_low,
        "median_ci95_high": ci_high,
        "minimum": min(values),
        "maximum": max(values),
    }


def execute_scenario(
    args: argparse.Namespace,
    binaries: dict[str, Path],
    case_name: str,
    producers: int,
) -> dict[str, Any]:
    capacity = args.capacity if case_name == "bounded" else 0
    messages = args.initial_messages
    calibration_rows: list[dict[str, Any]] = []
    calibration_floor = args.target_seconds * 1.50

    for attempt in range(args.calibration_attempts):
        probe_rows = []
        for probe in range(args.calibration_probes):
            order_index = attempt * args.calibration_probes + probe
            rows = run_pair(
                binaries,
                case_name,
                capacity,
                messages,
                producers,
                args.producer_core,
                args.consumer_core,
                order_index,
                args.process_timeout,
            )
            for row in rows:
                row.update(phase="calibration", sample_index=probe, attempt=attempt)
            calibration_rows.extend(rows)
            probe_rows.extend(rows)
        shortest = min(row["elapsed_ns"] for row in probe_rows) / 1_000_000_000.0
        if shortest >= calibration_floor:
            break
        scale = calibration_floor / shortest
        next_messages = math.ceil(messages * scale)
        if next_messages <= messages:
            next_messages = messages + 1
        messages = next_messages
        if messages > args.max_messages:
            raise RuntimeError(
                f"{case_name} {producers}p1c calibration requires more than "
                f"{args.max_messages} messages"
            )
    else:
        raise RuntimeError(f"{case_name} {producers}p1c failed to calibrate in time")

    warmup_rows: list[dict[str, Any]] = []
    for warmup in range(args.warmups):
        rows = run_pair(
            binaries,
            case_name,
            capacity,
            messages,
            producers,
            args.producer_core,
            args.consumer_core,
            warmup,
            args.process_timeout,
        )
        for row in rows:
            row.update(phase="warmup", sample_index=warmup, attempt=None)
        warmup_rows.extend(rows)

    sample_rows: list[dict[str, Any]] = []
    for sample in range(args.samples):
        rows = run_pair(
            binaries,
            case_name,
            capacity,
            messages,
            producers,
            args.producer_core,
            args.consumer_core,
            sample,
            args.process_timeout,
        )
        for row in rows:
            row.update(phase="sample", sample_index=sample, attempt=None)
            if row["elapsed_ns"] < args.target_seconds * 1_000_000_000:
                raise RuntimeError(
                    f"{case_name} {producers}p1c {row['language']} sample {sample} "
                    f"lasted less than {args.target_seconds:.6f}s"
                )
        sample_rows.extend(rows)

    by_language = {
        language: [
            row["messages_per_second"]
            for row in sample_rows
            if row["language"] == language
        ]
        for language in LANGUAGES
    }
    paired_ratios = []
    ratios_by_run_order = {"cpp_first": [], "rust_first": []}
    for sample in range(args.samples):
        pair = [row for row in sample_rows if row["sample_index"] == sample]
        cpp = next(
            row for row in pair if row["language"] == "cpp"
        )
        rust = next(
            row for row in pair if row["language"] == "rust"
        )
        ratio = cpp["messages_per_second"] / rust["messages_per_second"]
        paired_ratios.append(ratio)
        ratios_by_run_order[f"{pair[0]['language']}_first"].append(ratio)

    seed = (
        sum(ord(character) for character in case_name)
        + capacity
        + producers * 100_003
    )
    paired_summary = summarize(paired_ratios, args.bootstrap_resamples, seed + 2)
    block_ci_low, block_ci_high = bootstrap_abba_block_median_ci(
        paired_ratios, args.bootstrap_resamples, seed + 3
    )
    paired_summary.update(
        abba_block_median_ci95_low=block_ci_low,
        abba_block_median_ci95_high=block_ci_high,
    )
    summary = {
        "cpp_messages_per_second": summarize(
            by_language["cpp"], args.bootstrap_resamples, seed
        ),
        "rust_messages_per_second": summarize(
            by_language["rust"], args.bootstrap_resamples, seed + 1
        ),
        "paired_cpp_over_rust": paired_summary,
        "run_order_diagnostics": {
            order: {
                "samples": len(values),
                "median": statistics.median(values),
                "mean": statistics.fmean(values),
                "minimum": min(values),
                "maximum": max(values),
            }
            for order, values in ratios_by_run_order.items()
        },
    }
    acceptance = {
        "minimum_speedup": args.minimum_speedup,
        "max_cv_percent": args.max_cv_percent,
        "paired_ratio_abba_block_ci95_low": block_ci_low,
        "paired_ratio_ci_passed": block_ci_low > args.minimum_speedup,
        "cpp_cv_passed": summary["cpp_messages_per_second"]["cv_percent"]
        <= args.max_cv_percent,
        "rust_cv_passed": summary["rust_messages_per_second"]["cv_percent"]
        <= args.max_cv_percent,
    }
    acceptance["passed"] = all(
        acceptance[field]
        for field in ("paired_ratio_ci_passed", "cpp_cv_passed", "rust_cv_passed")
    )
    return {
        "case": case_name,
        "topology": f"{producers}p1c",
        "producers": producers,
        "capacity": capacity,
        "messages": messages,
        "calibration": calibration_rows,
        "warmups": warmup_rows,
        "samples": sample_rows,
        "summary": summary,
        "acceptance": acceptance,
    }


def write_outputs(report: dict[str, Any], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_json = output_dir / "mpsc_paired_raw.json"
    summary_json = output_dir / "mpsc_paired_summary.json"
    raw_csv = output_dir / "mpsc_paired_raw.csv"

    raw_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    summary = {
        "schema": report["schema"],
        "generated_at": report["generated_at"],
        "config": report["config"],
        "environment": report["environment"],
        "scenarios": [
            {
                "case": scenario["case"],
                "topology": scenario["topology"],
                "producers": scenario["producers"],
                "capacity": scenario["capacity"],
                "messages": scenario["messages"],
                "summary": scenario["summary"],
                "acceptance": scenario["acceptance"],
            }
            for scenario in report["scenarios"]
        ],
    }
    summary_json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    fieldnames = [
        "phase",
        "attempt",
        "sample_index",
        "language",
        "case",
        "topology",
        "ordering",
        "capacity",
        "payload_bytes",
        "messages",
        "elapsed_ns",
        "messages_per_second",
        "received",
        "checksum",
        "expected_checksum",
        "fifo_ok",
        "full_retries",
        "empty_retries",
        "producer_placement",
        "consumer_placement",
        "backoff",
        "generator",
        "valid",
    ]
    with raw_csv.open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for scenario in report["scenarios"]:
            for section in ("calibration", "warmups", "samples"):
                writer.writerows(scenario[section])


def main() -> int:
    args = parse_args()
    lock_path = Path("/tmp/galay_mpsc_paired.lock")
    try:
        benchmark_lock = lock_path.open("w")
        fcntl.flock(benchmark_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except (OSError, BlockingIOError) as error:
        print(f"another MPSC benchmark owns {lock_path}: {error}", file=sys.stderr)
        return 2
    try:
        binaries = {
            "cpp": args.cpp_binary.resolve(),
            "rust": args.rust_binary.resolve(),
        }
        for language, binary in binaries.items():
            if not binary.is_file():
                print(f"{language} binary not found: {binary}", file=sys.stderr)
                return 2

        report = {
            "schema": "galay.mpsc.paired.report.v1",
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "config": {
                "cases": args.cases,
                "producers": args.producers,
                "topologies": [f"{producers}p1c" for producers in args.producers],
                "capacity": args.capacity,
                "initial_messages": args.initial_messages,
                "max_messages": args.max_messages,
                "target_seconds": args.target_seconds,
                "calibration_probes": args.calibration_probes,
                "calibration_attempts": args.calibration_attempts,
                "warmups": args.warmups,
                "samples": args.samples,
                "bootstrap_resamples": args.bootstrap_resamples,
                "process_timeout": args.process_timeout,
                "minimum_speedup": args.minimum_speedup,
                "max_cv_percent": args.max_cv_percent,
                "producer_core": args.producer_core,
                "consumer_core": args.consumer_core,
                "payload_bytes": 8,
                "ordering": "producer_fifo",
                "backoff": "yield",
                "generator": "per_producer_monotonic_u64",
            },
            "environment": {
                "platform": platform.platform(),
                "machine": platform.machine(),
                "python": platform.python_version(),
                "logical_cpu_count": os.cpu_count(),
                "binaries": {
                    language: {
                        "path": str(binary),
                        "sha256": binary_sha256(binary),
                    }
                    for language, binary in binaries.items()
                },
            },
            "scenarios": [],
        }

        for case_name in args.cases:
            for producers in args.producers:
                report["scenarios"].append(
                    execute_scenario(args, binaries, case_name, producers)
                )
        write_outputs(report, args.output_dir)
    except (OSError, RuntimeError) as error:
        print(f"paired benchmark failed: {error}", file=sys.stderr)
        return 1

    for scenario in report["scenarios"]:
        print(
            json.dumps(
                {
                    "case": scenario["case"],
                    "topology": scenario["topology"],
                    "capacity": scenario["capacity"],
                    "messages": scenario["messages"],
                    "summary": scenario["summary"],
                    "acceptance": scenario["acceptance"],
                },
                sort_keys=True,
            )
        )
    print(f"raw_json={args.output_dir / 'mpsc_paired_raw.json'}")
    print(f"raw_csv={args.output_dir / 'mpsc_paired_raw.csv'}")
    print(f"summary_json={args.output_dir / 'mpsc_paired_summary.json'}")
    return 0 if all(
        scenario["acceptance"]["passed"] for scenario in report["scenarios"]
    ) else 4


if __name__ == "__main__":
    raise SystemExit(main())
