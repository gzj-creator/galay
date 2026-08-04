#!/usr/bin/env python3
"""Run paired C++/Rust SPSC benchmarks with one shared workload."""

from __future__ import annotations

import argparse
import csv
import fcntl
import hashlib
import json
import math
import platform
import random
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


LANGUAGES = ("cpp", "rust")
REQUIRED_FIELDS = {
    "schema",
    "language",
    "case",
    "implementation",
    "api_profile",
    "comparison_scope",
    "topology",
    "payload_bytes",
    "capacity",
    "batch_size",
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

EXPECTED_IMPLEMENTATIONS = {
    ("cpp", "raw_bounded"): "galay::spsc::Ring::split",
    ("rust", "raw_bounded"): "rtrb::RingBuffer@0.3.4",
    ("cpp", "batch_bounded"): "galay::spsc::Ring::split batch",
    ("rust", "batch_bounded"): "rtrb::RingBuffer@0.3.4",
    ("cpp", "unbounded"): "galay::spsc::UnboundedChannel",
    ("rust", "unbounded"): "unbounded_spsc::channel@0.3.0",
    ("cpp", "batch_unbounded"): (
        "galay::spsc::UnboundedChannel::sendBatch/tryRecvBatch"
    ),
    ("rust", "batch_unbounded"): (
        "unbounded_spsc::channel@0.3.0 scalar batch emulation"
    ),
}

API_PROFILES = {
    "raw_bounded": ("bounded_spsc_polling_split", "equivalent_measured_api"),
    "batch_bounded": (
        "bounded_spsc_batch_polling_split",
        "equivalent_measured_api",
    ),
    "unbounded": (
        "unbounded_spsc_wait_capable_polling_path",
        "nearest_available_measured_path",
    ),
    "batch_unbounded": (
        "unbounded_spsc_batch_polling",
        "reference_only_no_equivalent_rust_batch_api",
    ),
}

BOUNDED_CASES = {"raw_bounded", "batch_bounded"}
BATCH_CASES = {"batch_bounded", "batch_unbounded"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Calibrate and run paired Galay C++ / Rust SPSC benchmarks."
    )
    parser.add_argument("--cpp-binary", type=Path, required=True)
    parser.add_argument("--rust-binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--cases",
        default="raw_bounded,batch_bounded",
        help=(
            "comma-separated subset of raw_bounded,batch_bounded,unbounded,"
            "batch_unbounded"
        ),
    )
    parser.add_argument("--capacity", type=int, default=4096)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument(
        "--backoff", choices=("yield", "spin", "hybrid"), default="yield"
    )
    parser.add_argument("--initial-messages", type=int, default=1_000_000)
    parser.add_argument("--max-messages", type=int, default=2_000_000_000)
    parser.add_argument("--target-seconds", type=float, default=1.0)
    parser.add_argument("--calibration-probes", type=int, default=3)
    parser.add_argument("--calibration-attempts", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--samples", type=int, default=15)
    parser.add_argument("--bootstrap-resamples", type=int, default=10_000)
    parser.add_argument("--process-timeout", type=float, default=300.0)
    parser.add_argument("--minimum-speedup", type=float, default=1.05)
    parser.add_argument("--max-cv-percent", type=float, default=25.0)
    parser.add_argument("--max-retry-ratio", type=float, default=10.0)
    parser.add_argument("--producer-core", type=int, default=0)
    parser.add_argument("--consumer-core", type=int, default=1)
    parser.add_argument("--cpp-compiler", default="c++")
    parser.add_argument("--rust-compiler", default="rustc")
    parser.add_argument("--rust-toolchain", default="nightly")
    args = parser.parse_args()

    allowed_cases = BOUNDED_CASES | {"unbounded", "batch_unbounded"}
    cases = [value.strip() for value in args.cases.split(",") if value.strip()]
    if not cases or any(value not in allowed_cases for value in cases):
        parser.error(
            "--cases must contain raw_bounded, batch_bounded, unbounded, "
            "and/or batch_unbounded"
        )
    if len(set(cases)) != len(cases):
        parser.error("--cases must not contain duplicates")
    if args.capacity < 2 or args.capacity & (args.capacity - 1):
        parser.error("--capacity must be a power of two and at least 2")
    if args.batch_size <= 0:
        parser.error("--batch-size must be positive")
    if args.initial_messages <= 0 or args.max_messages < args.initial_messages:
        parser.error("message bounds are invalid")
    if args.target_seconds < 0.0001:
        parser.error("--target-seconds must be at least 0.0001")
    if args.calibration_probes <= 0 or args.calibration_attempts <= 0:
        parser.error("calibration counts must be positive")
    if args.warmups < 1 or args.samples < 3:
        parser.error("at least one warmup and three samples are required")
    if args.bootstrap_resamples < 1_000:
        parser.error("--bootstrap-resamples must be at least 1000")
    if args.process_timeout <= args.target_seconds:
        parser.error("--process-timeout must exceed --target-seconds")
    if (
        args.minimum_speedup <= 1.0
        or args.max_cv_percent <= 0.0
        or args.max_retry_ratio < 1.0
    ):
        parser.error("acceptance thresholds are invalid")
    if args.producer_core < 0 or args.consumer_core < 0:
        parser.error("core indices must be non-negative")
    if args.producer_core == args.consumer_core:
        parser.error("producer and consumer must use different core indices")

    args.cases = cases
    return args


def binary_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as binary:
        while chunk := binary.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def command_version(command: list[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            check=False,
            timeout=10.0,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"failed to query {' '.join(command)}: {error}") from error
    if completed.returncode != 0:
        raise RuntimeError(
            f"version command {' '.join(command)} failed: "
            f"{completed.stderr.strip() or completed.stdout.strip()}"
        )
    return completed.stdout.strip()


def run_once(
    binary: Path,
    language: str,
    case_name: str,
    capacity: int,
    batch_size: int,
    backoff: str,
    messages: int,
    producer_core: int,
    consumer_core: int,
    process_timeout: float,
    allow_invalid: bool,
) -> dict[str, Any]:
    command = [
        str(binary),
        "--case",
        case_name,
        "--capacity",
        str(capacity),
        "--batch-size",
        str(batch_size),
        "--backoff",
        backoff,
        "--messages",
        str(messages),
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
            f"{language} benchmark exceeded {process_timeout:.1f}s"
        ) from error
    if completed.returncode not in (0, 1):
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

    missing = REQUIRED_FIELDS.difference(result)
    if missing:
        raise RuntimeError(f"{language} result misses fields: {sorted(missing)}")
    expected_capacity = capacity if case_name in BOUNDED_CASES else 0
    expected_batch_size = batch_size if case_name in BATCH_CASES else 1
    api_profile, comparison_scope = API_PROFILES[case_name]
    expected = {
        "schema": "galay.spsc.paired.v4",
        "language": language,
        "case": case_name,
        "implementation": EXPECTED_IMPLEMENTATIONS[(language, case_name)],
        "api_profile": api_profile,
        "comparison_scope": comparison_scope,
        "topology": "1p1c",
        "payload_bytes": 8,
        "capacity": expected_capacity,
        "batch_size": expected_batch_size,
        "messages": messages,
        "backoff": backoff,
        "generator": "monotonic_u64",
    }
    for key, expected_value in expected.items():
        if result[key] != expected_value:
            raise RuntimeError(
                f"{language} field {key}={result[key]!r}, expected {expected_value!r}"
            )
    if result["elapsed_ns"] <= 0 or result["messages_per_second"] <= 0:
        raise RuntimeError(f"{language} reported a non-positive measurement")
    result["process_returncode"] = completed.returncode
    result["correctness_passed"] = (
        completed.returncode == 0
        and result["valid"] is True
        and result["fifo_ok"] is True
        and result["received"] == messages
        and result["checksum"] == result["expected_checksum"]
    )
    if not result["correctness_passed"] and not allow_invalid:
        raise RuntimeError(
            f"{language} correctness failed: received={result['received']} "
            f"expected={messages} fifo_ok={result['fifo_ok']} "
            f"checksum={result['checksum']} "
            f"expected_checksum={result['expected_checksum']} "
            f"valid={result['valid']} returncode={completed.returncode}"
        )
    result["command"] = command
    return result


def language_order(index: int) -> tuple[str, str]:
    # ABBA ordering balances first-run, thermal, and short-term frequency bias.
    return LANGUAGES if index % 4 in (0, 3) else tuple(reversed(LANGUAGES))


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


def summarize(values: list[float], resamples: int, seed: int) -> dict[str, float]:
    mean = statistics.fmean(values)
    deviation = statistics.stdev(values)
    ci_low, ci_high = bootstrap_median_ci(values, resamples, seed)
    return {
        "p50": statistics.median(values),
        "p99": percentile(values, 0.99),
        "median": statistics.median(values),
        "mean": mean,
        "cv_percent": deviation * 100.0 / mean if mean != 0.0 else 0.0,
        "median_ci95_low": ci_low,
        "median_ci95_high": ci_high,
        "minimum": min(values),
        "maximum": max(values),
    }


def execute_case(
    args: argparse.Namespace,
    binaries: dict[str, Path],
    case_name: str,
) -> dict[str, Any]:
    capacity = args.capacity if case_name in BOUNDED_CASES else 0
    messages = args.initial_messages
    calibration_rows: list[dict[str, Any]] = []
    _, comparison_scope = API_PROFILES[case_name]
    comparison_eligible = comparison_scope == "equivalent_measured_api"
    # Formal equivalent comparisons keep the 3x floor that absorbs short-run
    # frequency variance. Reference-only paths use the requested duration
    # directly so a much slower non-equivalent library cannot make each paired
    # sample exceed the process timeout.
    calibration_floor_multiplier = 3.0 if comparison_eligible else 1.0
    calibration_floor = args.target_seconds * calibration_floor_multiplier

    for attempt in range(args.calibration_attempts):
        probe_rows = []
        for probe in range(args.calibration_probes):
            for language in language_order(probe):
                row = run_once(
                    binaries[language],
                    language,
                    case_name,
                    capacity,
                    args.batch_size,
                    args.backoff,
                    messages,
                    args.producer_core,
                    args.consumer_core,
                    args.process_timeout,
                    not comparison_eligible,
                )
                row.update(phase="calibration", sample_index=probe, attempt=attempt)
                calibration_rows.append(row)
                probe_rows.append(row)
        shortest = min(row["elapsed_ns"] for row in probe_rows) / 1_000_000_000.0
        if shortest >= calibration_floor:
            break
        scale = calibration_floor / shortest
        next_messages = int(math.ceil(messages * scale / 1_000_000.0)) * 1_000_000
        if next_messages <= messages:
            next_messages = messages + 1_000_000
        messages = next_messages
        if messages > args.max_messages:
            raise RuntimeError(
                f"{case_name} calibration requires more than {args.max_messages} messages"
            )
    else:
        raise RuntimeError(f"{case_name} failed to calibrate in time")

    warmup_rows: list[dict[str, Any]] = []
    for warmup in range(args.warmups):
        for language in language_order(warmup):
            row = run_once(
                binaries[language],
                language,
                case_name,
                capacity,
                args.batch_size,
                args.backoff,
                messages,
                args.producer_core,
                args.consumer_core,
                args.process_timeout,
                not comparison_eligible,
            )
            row.update(phase="warmup", sample_index=warmup, attempt=None)
            warmup_rows.append(row)

    sample_rows: list[dict[str, Any]] = []
    placements: dict[tuple[int, str], tuple[str, str]] = {}
    for sample in range(args.samples):
        for language in language_order(sample):
            row = run_once(
                binaries[language],
                language,
                case_name,
                capacity,
                args.batch_size,
                args.backoff,
                messages,
                args.producer_core,
                args.consumer_core,
                args.process_timeout,
                not comparison_eligible,
            )
            row.update(phase="sample", sample_index=sample, attempt=None)
            if row["elapsed_ns"] < args.target_seconds * 1_000_000_000:
                raise RuntimeError(
                    f"{case_name} {language} sample {sample} lasted less than "
                    f"{args.target_seconds:.3f}s"
                )
            placements[(sample, language)] = (
                row["producer_placement"],
                row["consumer_placement"],
            )
            sample_rows.append(row)

    for sample in range(args.samples):
        if placements[(sample, "cpp")] != placements[(sample, "rust")]:
            raise RuntimeError(
                f"{case_name} sample {sample} used different C++/Rust placement"
            )
    placement_passed = all(
        producer == "pinned" and consumer == "pinned"
        for producer, consumer in placements.values()
    )

    by_language = {
        language: [
            row["messages_per_second"]
            for row in sample_rows
            if row["language"] == language
        ]
        for language in LANGUAGES
    }
    paired_ratios = []
    for sample in range(args.samples):
        cpp = next(
            row for row in sample_rows
            if row["sample_index"] == sample and row["language"] == "cpp"
        )
        rust = next(
            row for row in sample_rows
            if row["sample_index"] == sample and row["language"] == "rust"
        )
        paired_ratios.append(cpp["messages_per_second"] / rust["messages_per_second"])

    retry_medians = {
        language: statistics.median(
            row["empty_retries"]
            for row in sample_rows
            if row["language"] == language
        )
        for language in LANGUAGES
    }
    retry_ratio = max(retry_medians.values()) / max(1.0, min(retry_medians.values()))

    retry_ratios = {
        language: {
            "full": [
                row["full_retries"] / messages
                for row in sample_rows
                if row["language"] == language
            ],
            "empty": [
                row["empty_retries"] / messages
                for row in sample_rows
                if row["language"] == language
            ],
            "total": [
                (row["full_retries"] + row["empty_retries"]) / messages
                for row in sample_rows
                if row["language"] == language
            ],
        }
        for language in LANGUAGES
    }

    seed = sum(ord(character) for character in case_name) + capacity
    summary = {
        "cpp_messages_per_second": summarize(
            by_language["cpp"], args.bootstrap_resamples, seed
        ),
        "rust_messages_per_second": summarize(
            by_language["rust"], args.bootstrap_resamples, seed + 1
        ),
        "paired_cpp_over_rust": summarize(
            paired_ratios, args.bootstrap_resamples, seed + 2
        ),
        "empty_retries": {
            "cpp_median": retry_medians["cpp"],
            "rust_median": retry_medians["rust"],
            "max_over_min_floor_one": retry_ratio,
        },
        "retry_ratio": {
            language: {
                retry_kind: summarize(
                    values,
                    args.bootstrap_resamples,
                    seed + 10 + language_index * 3 + retry_index,
                )
                for retry_index, (retry_kind, values) in enumerate(
                    retry_ratios[language].items()
                )
            }
            for language_index, language in enumerate(LANGUAGES)
        },
    }
    ratio_summary = summary["paired_cpp_over_rust"]
    measured_rows = calibration_rows + warmup_rows + sample_rows
    acceptance = {
        "minimum_speedup": args.minimum_speedup,
        "max_cv_percent": args.max_cv_percent,
        "max_retry_ratio": args.max_retry_ratio,
        "ratio_ci95_low_passed": (
            ratio_summary["median_ci95_low"] > args.minimum_speedup
        ),
        "cpp_cv_passed": (
            summary["cpp_messages_per_second"]["cv_percent"] <= args.max_cv_percent
        ),
        "rust_cv_passed": (
            summary["rust_messages_per_second"]["cv_percent"] <= args.max_cv_percent
        ),
        "placement_passed": placement_passed,
        "retry_ratio_passed": retry_ratio <= args.max_retry_ratio,
        "correctness_passed": all(
            row["correctness_passed"] for row in measured_rows
        ),
        "comparison_eligible": comparison_eligible,
    }
    acceptance["stability_passed"] = all(
        acceptance[key]
        for key in (
            "cpp_cv_passed",
            "rust_cv_passed",
            "placement_passed",
            "retry_ratio_passed",
        )
    )
    acceptance["passed"] = (
        acceptance["correctness_passed"]
        and acceptance["stability_passed"]
        and acceptance["ratio_ci95_low_passed"]
        and acceptance["comparison_eligible"]
    )
    return {
        "case": case_name,
        "capacity": capacity,
        "batch_size": args.batch_size if case_name in BATCH_CASES else 1,
        "backoff": args.backoff,
        "messages": messages,
        "calibration_floor_seconds": calibration_floor,
        "calibration_floor_multiplier": calibration_floor_multiplier,
        "calibration": calibration_rows,
        "warmups": warmup_rows,
        "samples": sample_rows,
        "summary": summary,
        "acceptance": acceptance,
    }


def write_outputs(report: dict[str, Any], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_json = output_dir / "spsc_paired_raw.json"
    summary_json = output_dir / "spsc_paired_summary.json"
    raw_csv = output_dir / "spsc_paired_raw.csv"

    raw_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    summary = {
        "schema": report["schema"],
        "generated_at": report["generated_at"],
        "config": report["config"],
        "environment": report["environment"],
        "cases": [
            {
                "case": case["case"],
                "capacity": case["capacity"],
                "batch_size": case["batch_size"],
                "backoff": case["backoff"],
                "messages": case["messages"],
                "calibration_floor_seconds": case["calibration_floor_seconds"],
                "calibration_floor_multiplier": case["calibration_floor_multiplier"],
                "summary": case["summary"],
                "acceptance": case["acceptance"],
            }
            for case in report["cases"]
        ],
    }
    summary_json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    fieldnames = [
        "phase", "attempt", "sample_index", "language", "case",
        "implementation", "api_profile", "comparison_scope", "capacity", "batch_size",
        "payload_bytes", "messages", "elapsed_ns", "messages_per_second",
        "received", "checksum", "expected_checksum", "fifo_ok", "full_retries",
        "empty_retries", "producer_placement", "consumer_placement", "backoff",
        "generator", "valid",
        "process_returncode", "correctness_passed",
    ]
    with raw_csv.open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for case in report["cases"]:
            for section in ("calibration", "warmups", "samples"):
                writer.writerows(case[section])


def main() -> int:
    args = parse_args()
    lock_path = Path("/tmp/galay_spsc_paired.lock")
    try:
        benchmark_lock = lock_path.open("w")
        fcntl.flock(benchmark_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except (OSError, BlockingIOError) as error:
        print(f"another SPSC benchmark owns {lock_path}: {error}", file=sys.stderr)
        return 2
    binaries = {"cpp": args.cpp_binary.resolve(), "rust": args.rust_binary.resolve()}
    for language, binary in binaries.items():
        if not binary.is_file():
            print(f"{language} binary not found: {binary}", file=sys.stderr)
            return 2
    try:
        compilers = {
            "cpp": command_version([args.cpp_compiler, "--version"]),
            "rust": command_version(
                [args.rust_compiler, f"+{args.rust_toolchain}", "-vV"]
            ),
        }
    except RuntimeError as error:
        print(f"paired benchmark failed: {error}", file=sys.stderr)
        return 1

    report = {
        "schema": "galay.spsc.paired.report.v4",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "config": {
            "cases": args.cases,
            "capacity": args.capacity,
            "batch_size": args.batch_size,
            "target_seconds": args.target_seconds,
            "warmups": args.warmups,
            "samples": args.samples,
            "bootstrap_resamples": args.bootstrap_resamples,
            "process_timeout": args.process_timeout,
            "minimum_speedup": args.minimum_speedup,
            "max_cv_percent": args.max_cv_percent,
            "max_retry_ratio": args.max_retry_ratio,
            "producer_core": args.producer_core,
            "consumer_core": args.consumer_core,
            "payload_bytes": 8,
            "topology": "1p1c",
            "backoff": args.backoff,
            "generator": "monotonic_u64",
        },
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": platform.python_version(),
            "compilers": compilers,
            "binaries": {
                language: {
                    "path": str(binary),
                    "sha256": binary_sha256(binary),
                }
                for language, binary in binaries.items()
            },
        },
        "cases": [],
    }

    try:
        for case_name in args.cases:
            report["cases"].append(execute_case(args, binaries, case_name))
        write_outputs(report, args.output_dir)
    except (OSError, RuntimeError) as error:
        print(f"paired benchmark failed: {error}", file=sys.stderr)
        return 1

    for case in report["cases"]:
        print(json.dumps({
            "case": case["case"],
            "capacity": case["capacity"],
            "batch_size": case["batch_size"],
            "backoff": case["backoff"],
            "messages": case["messages"],
            "calibration_floor_seconds": case["calibration_floor_seconds"],
            "calibration_floor_multiplier": case["calibration_floor_multiplier"],
            "summary": case["summary"],
            "acceptance": case["acceptance"],
        }, sort_keys=True))
    print(f"raw_json={args.output_dir / 'spsc_paired_raw.json'}")
    print(f"raw_csv={args.output_dir / 'spsc_paired_raw.csv'}")
    print(f"summary_json={args.output_dir / 'spsc_paired_summary.json'}")
    return 0 if all(case["acceptance"]["passed"] for case in report["cases"]) else 4


if __name__ == "__main__":
    raise SystemExit(main())
