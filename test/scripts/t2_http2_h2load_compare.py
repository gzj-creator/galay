#!/usr/bin/env python3
"""Verify protocol comparison scripts fail closed to the Asio policy."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "http2" / "300_http2_h2load_compare.sh"


def main() -> int:
    env = os.environ.copy()
    proc = subprocess.run(
        ["bash", str(SCRIPT), "--post-echo"],
        cwd=str(ROOT),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        raise AssertionError(f"policy probe should succeed: {proc.stderr}")
    expected = {
        "status=not_applicable",
        "competitor=boost.asio coroutine",
        "scenario=http2 protocol",
    }
    actual = set(proc.stdout.splitlines())
    missing = expected - actual
    if missing:
        raise AssertionError(f"policy probe missing lines: {sorted(missing)}")
    competitor_lines = [line for line in proc.stdout.splitlines() if line.startswith("competitor=")]
    if competitor_lines != ["competitor=boost.asio coroutine"]:
        raise AssertionError("external HTTP/2 tools must not be reported as competitors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
