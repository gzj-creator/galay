#!/usr/bin/env python3
"""Tests for the HTTP/2 h2load comparison script guardrails."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "http2" / "300_http2_h2load_compare.sh"


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_fake_command(directory: Path, name: str) -> None:
    path = directory / name
    path.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    path.chmod(0o755)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="galay-http2-script-test-") as tmp:
        tmp_path = Path(tmp)
        build_dir = tmp_path / "build-debug"
        bin_dir = tmp_path / "bin"
        build_dir.mkdir()
        bin_dir.mkdir()

        (build_dir / "CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=Debug\n",
            encoding="utf-8",
        )
        write_fake_command(bin_dir, "h2load")
        write_fake_command(bin_dir, "nghttpd")

        env = os.environ.copy()
        env["BUILD_DIR"] = str(build_dir)
        env["PATH"] = f"{bin_dir}{os.pathsep}{env.get('PATH', '')}"
        env.pop("ALLOW_NON_RELEASE", None)

        proc = subprocess.run(
            ["bash", str(SCRIPT), "--post-echo-best"],
            cwd=str(ROOT),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        check(proc.returncode == 1, "best-of mode should reject a Debug build")
        check(
            "best-of mode requires a Release galay build" in proc.stderr,
            "stderr should explain the Release-build requirement",
        )
        check(
            f"current BUILD_DIR: {build_dir}" in proc.stderr,
            "stderr should include the selected BUILD_DIR",
        )
        check(
            "current CMAKE_BUILD_TYPE: Debug" in proc.stderr,
            "stderr should include the detected CMAKE_BUILD_TYPE",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
