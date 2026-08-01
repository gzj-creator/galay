#!/usr/bin/env python3
"""Run paired Galay C++ and Rust Crossbeam unbounded MPMC benchmarks."""

from __future__ import annotations

import argparse
import json
import math
import random
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any


REQUIRED_FIELDS = {
    "schema",
    "language",
    "case",
    "path",
    "topology",
    "payload_bytes",
    "messages",
    "elapsed_ns",
    "messages_per_second",
    "received",
    "checksum",
    "expected_checksum",
    "send_retries",
    "empty_retries",
    "placement",
    "backoff",
    "generator",
    "valid",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-binary", type=Path, required=True)
    parser.add_argument("--rust-binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--messages", type=int, default=5_000_000)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--samples", type=int, default=15)
    parser.add_argument("--bootstrap-resamples", type=int, default=10_000)
    parser.add_argument("--seed", type=int, default=20260801)
    parser.add_argument("--process-timeout", type=float, default=60.0)
    args = parser.parse_args()
    if args.messages <= 0 or args.messages > 2**32:
        parser.error("--messages must be in 1..=2^32")
    if args.warmups < 1 or args.samples < 15:
        parser.error("at least one warmup and 15 paired samples are required")
    if args.bootstrap_resamples < 1000:
        parser.error("--bootstrap-resamples must be at least 1000")
    if not math.isfinite(args.process_timeout) or args.process_timeout <= 0:
        parser.error("--process-timeout must be finite and positive")
    return args


def run_once(
    binary: Path,
    language: str,
    workers: int,
    messages: int,
    timeout: float,
) -> dict[str, Any]:
    command = [
        str(binary),
        "--messages",
        str(messages),
        "--producers",
        str(workers),
        "--consumers",
        str(workers),
    ]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"{language} {workers}p{workers}c timed out") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"{language} benchmark failed: {detail}")
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise RuntimeError(f"{language} benchmark must emit one JSON line")
    result = json.loads(lines[0])
    if not isinstance(result, dict):
        raise RuntimeError(f"{language} result must be a JSON object")
    missing = REQUIRED_FIELDS.difference(result)
    if missing:
        raise RuntimeError(f"{language} result misses fields: {sorted(missing)}")
    expected = {
        "schema": "galay.mpmc.paired.v1",
        "language": language,
        "case": "unbounded",
        "path": "token",
        "topology": f"{workers}p{workers}c",
        "payload_bytes": 8,
        "messages": messages,
        "received": messages,
        "backoff": "yield",
        "generator": "partitioned_monotonic_u64",
        "valid": True,
    }
    for field, value in expected.items():
        if result[field] != value:
            raise RuntimeError(
                f"{language} {workers}p{workers}c field {field}="
                f"{result[field]!r}, expected {value!r}"
            )
    if result["checksum"] != result["expected_checksum"]:
        raise RuntimeError(f"{language} checksum mismatch")
    if result["send_retries"] != 0:
        raise RuntimeError(f"{language} reported send retries")
    if result["placement"] not in {"pinned", "perf-class-only"}:
        raise RuntimeError(f"{language} placement is unsupported")
    if result["elapsed_ns"] <= 0 or result["messages_per_second"] <= 0:
        raise RuntimeError(f"{language} reported a non-positive measurement")
    return result


def percentile(sorted_values: list[float], probability: float) -> float:
    index = probability * (len(sorted_values) - 1)
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return sorted_values[lower]
    weight = index - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def bootstrap_median_ci(
    values: list[float], resamples: int, seed: int
) -> tuple[float, float]:
    rng = random.Random(seed)
    medians = []
    for _ in range(resamples):
        sample = [values[rng.randrange(len(values))] for _ in values]
        medians.append(statistics.median(sample))
    medians.sort()
    return percentile(medians, 0.025), percentile(medians, 0.975)


def main() -> int:
    args = parse_args()
    for path in (args.cpp_binary, args.rust_binary):
        if not path.is_file():
            raise RuntimeError(f"benchmark binary not found: {path}")

    raw: dict[str, Any] = {
        "schema": "galay.mpmc.paired.raw.v1",
        "messages": args.messages,
        "warmups": args.warmups,
        "samples": args.samples,
        "topologies": {},
    }
    summary: dict[str, Any] = {
        "schema": "galay.mpmc.paired.summary.v1",
        "messages": args.messages,
        "samples": args.samples,
        "topologies": {},
    }

    for workers in (2, 4):
        for warmup in range(args.warmups):
            order = ("cpp", "rust") if warmup % 2 == 0 else ("rust", "cpp")
            for language in order:
                binary = args.cpp_binary if language == "cpp" else args.rust_binary
                run_once(binary, language, workers, args.messages, args.process_timeout)

        pairs = []
        for sample in range(args.samples):
            order = ("cpp", "rust") if sample % 2 == 0 else ("rust", "cpp")
            pair: dict[str, Any] = {"sample": sample}
            for language in order:
                binary = args.cpp_binary if language == "cpp" else args.rust_binary
                pair[language] = run_once(
                    binary,
                    language,
                    workers,
                    args.messages,
                    args.process_timeout,
                )
            pair["cpp_over_rust"] = (
                pair["cpp"]["messages_per_second"]
                / pair["rust"]["messages_per_second"]
            )
            pairs.append(pair)

        cpp_values = [pair["cpp"]["messages_per_second"] for pair in pairs]
        rust_values = [pair["rust"]["messages_per_second"] for pair in pairs]
        ratios = [pair["cpp_over_rust"] for pair in pairs]
        ci_low, ci_high = bootstrap_median_ci(
            ratios, args.bootstrap_resamples, args.seed + workers
        )
        topology = f"{workers}p{workers}c"
        raw["topologies"][topology] = pairs
        summary["topologies"][topology] = {
            "cpp_median_messages_per_second": statistics.median(cpp_values),
            "rust_median_messages_per_second": statistics.median(rust_values),
            "paired_cpp_over_rust_median": statistics.median(ratios),
            "paired_cpp_over_rust_ci95": [ci_low, ci_high],
            "cpp_pair_wins": sum(ratio > 1.0 for ratio in ratios),
        }
        print(
            f"{topology} cpp_median={statistics.median(cpp_values):.0f} "
            f"rust_median={statistics.median(rust_values):.0f} "
            f"paired_ratio={statistics.median(ratios):.4f} "
            f"ci95=[{ci_low:.4f},{ci_high:.4f}] "
            f"wins={sum(ratio > 1.0 for ratio in ratios)}/{len(ratios)}"
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.output_dir / "mpmc_paired_raw.json"
    summary_path = args.output_dir / "mpmc_paired_summary.json"
    raw_path.write_text(json.dumps(raw, indent=2) + "\n", encoding="utf-8")
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"raw_json={raw_path}")
    print(f"summary_json={summary_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"paired benchmark failed: {error}", file=sys.stderr)
        raise SystemExit(1)
