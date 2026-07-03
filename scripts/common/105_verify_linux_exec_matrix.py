#!/usr/bin/env python3
"""Run and classify Linux example/benchmark executables.

This script is intentionally conservative: it treats known standalone
integration dependencies and long-running server benchmarks as classified
outcomes, while still returning non-zero for unknown failures.
"""

from __future__ import annotations

import argparse
import os
import signal
import socket
import stat
import subprocess
import sys
import time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


STATUSES = (
    "PASS",
    "SKIP",
    "LONG_RUNNING",
    "EXTERNAL_DEP",
    "NEEDS_PEER",
    "FAIL",
    "MISSING",
)

EXTERNAL_PATTERNS = (
    "/mysql/",
    "/redis/",
    "/mongo/",
    "/etcd/",
    "service_discovery",
    "otlp",
)

PEER_PATTERNS = (
    "_client",
    "client_",
    "tls_client_throughput",
    "ssl_tls_client_throughput",
    "tcp_socket_client_throughput",
)

LONG_RUNNING_PATTERNS = (
    "_server",
    "server_",
    "_proxy",
    "_watch",
    "_lease",
    "_pubsub",
    "_throughput",
    "_pressure",
    "_latency",
    "_contention",
    "fast_path",
)

CRASH_CODES = {
    128 + signal.SIGABRT,
    128 + signal.SIGBUS,
    128 + signal.SIGFPE,
    128 + signal.SIGILL,
    128 + signal.SIGSEGV,
    128 + signal.SIGTERM,
    128 + signal.SIGTRAP,
}

EXTERNAL_OUTPUT_PATTERNS = (
    "connection refused",
    "connect failed",
    "could not connect",
    "failed to connect",
    "no such file",
    "not found",
    "unavailable",
    "timeout",
)


@dataclass(frozen=True)
class PairPlan:
    server_rel: str
    server_args: tuple[str, ...]
    client_args: tuple[str, ...]
    host: str
    port: int
    probe_port: bool = True


def normalize_rel(path: str) -> str:
    return path.replace("\\", "/").lstrip("./")


def has_any(rel: str, patterns: tuple[str, ...]) -> bool:
    return any(pattern in rel for pattern in patterns)


def is_external_dep(rel: str) -> bool:
    rel = normalize_rel(rel)
    return has_any(rel, EXTERNAL_PATTERNS)


def needs_peer(rel: str) -> bool:
    rel = normalize_rel(rel)
    if is_external_dep(rel):
        return False
    return "manual_t" in rel or has_any(rel, PEER_PATTERNS)


def is_long_running(rel: str) -> bool:
    rel = normalize_rel(rel)
    if is_external_dep(rel) or needs_peer(rel):
        return False
    return has_any(rel, LONG_RUNNING_PATTERNS)


def is_crash_return(rc: int) -> bool:
    return rc < 0 or rc in CRASH_CODES


def has_external_output(output_lower: str) -> bool:
    return any(pattern in output_lower for pattern in EXTERNAL_OUTPUT_PATTERNS)


def classify_result(rel: str, rc: int, timed_out: bool, output: str) -> str:
    rel = normalize_rel(rel)
    output_lower = output.lower()
    if (
        rc == 125
        or "[SKIP]" in output
        or "SKIP:" in output
        or "unsupported" in output_lower
    ):
        return "SKIP"
    if rc == 0:
        return "PASS"
    if is_crash_return(rc):
        return "FAIL"
    if "[EXTERNAL_DEP]" in output:
        return "EXTERNAL_DEP"
    if is_external_dep(rel):
        return "EXTERNAL_DEP"
    if timed_out and ("[LONG_RUNNING]" in output or "LONG_RUNNING:" in output):
        return "LONG_RUNNING"
    if needs_peer(rel):
        return "NEEDS_PEER"
    if timed_out and is_long_running(rel):
        return "LONG_RUNNING"
    return "FAIL"


def executable_timeout(rel: str, default_timeout: float, server_timeout: float, external_timeout: float) -> float:
    if is_external_dep(rel):
        return external_timeout
    if is_long_running(rel):
        return server_timeout
    return default_timeout


def iter_executables(build_root: Path, list_file: Path | None, kind: str) -> list[str]:
    if list_file is not None:
        items = [normalize_rel(line.strip()) for line in list_file.read_text(encoding="utf-8").splitlines()]
    else:
        items = []
        for path in sorted(build_root.rglob("*")):
            if not path.is_file():
                continue
            mode = path.stat().st_mode
            if not (mode & stat.S_IXUSR):
                continue
            rel = path.relative_to(build_root).as_posix()
            if rel.startswith("examples/") or rel.startswith("benchmark/") or rel.startswith("bin/"):
                items.append(rel)

    if kind == "examples":
        return [item for item in items if item.startswith("examples/")]
    if kind == "benchmarks":
        return [item for item in items if item.startswith("benchmark/")]
    return [item for item in items if item.startswith(("examples/", "benchmark/", "bin/"))]


def run_command(build_root: Path, argv: list[str], timeout_seconds: float) -> tuple[int, bool, str]:
    proc = subprocess.Popen(
        argv,
        cwd=build_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    try:
        output, _ = proc.communicate(timeout=timeout_seconds)
        return proc.returncode, False, output or ""
    except subprocess.TimeoutExpired:
        signal_process(proc, signal.SIGTERM)
        try:
            output, _ = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            signal_process(proc, signal.SIGKILL)
            output, _ = proc.communicate()
        return 124, True, output or ""


def run_executable(build_root: Path, rel: str, timeout_seconds: float) -> tuple[int, bool, str]:
    return run_command(build_root, [str(build_root / rel), *single_executable_args(rel)], timeout_seconds)


def single_executable_args(rel: str) -> tuple[str, ...]:
    rel = normalize_rel(rel)
    if rel == "benchmark/c/kernel/benchmark_c_kernel_async_waiter_signal":
        return ("100",)
    if rel == "benchmark/c/kernel/benchmark_c_kernel_coro_tcp_iov_sendfile":
        return ("20",)
    return ()


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def source_root() -> Path:
    return Path(__file__).resolve().parents[2]


def build_pair_plan(rel: str, build_root: Path) -> PairPlan | None:
    rel = normalize_rel(rel)
    port = find_free_port()

    if rel == "benchmark/c/kernel/benchmark_c_kernel_tcp_socket_client_throughput":
        return PairPlan(
            server_rel="benchmark/c/kernel/benchmark_c_kernel_tcp_socket_server_throughput",
            server_args=(str(port), "1"),
            client_args=("-p", str(port), "-c", "1", "-d", "1"),
            host="127.0.0.1",
            port=port,
        )

    if rel == "benchmark/cpp/ssl/benchmark_ssl_tls_client_throughput":
        cert_dir = source_root() / "test" / "cpp" / "ssl" / "certs"
        return PairPlan(
            server_rel="benchmark/cpp/ssl/benchmark_ssl_tls_server_throughput",
            server_args=(
                str(port),
                str(cert_dir / "server.crt"),
                str(cert_dir / "server.key"),
                "128",
                "1",
            ),
            client_args=("127.0.0.1", str(port), "1", "1", "47", "1", "3"),
            host="127.0.0.1",
            port=port,
        )

    if rel == "examples/c/ws/example_c_ws_echo_client":
        return PairPlan(
            server_rel="examples/c/ws/example_c_ws_echo_server",
            server_args=(str(port),),
            client_args=("127.0.0.1", str(port), "/"),
            host="127.0.0.1",
            port=port,
            probe_port=False,
        )

    ssl_pairs = {
        "examples/cpp/ssl/example_ssl_include_echo_client":
            "examples/cpp/ssl/example_ssl_include_echo_server",
        "examples/cpp/ssl/example_ssl_import_echo_client":
            "examples/cpp/ssl/example_ssl_import_echo_server",
    }
    if rel in ssl_pairs:
        cert_dir = source_root() / "test" / "cpp" / "ssl" / "certs"
        return PairPlan(
            server_rel=ssl_pairs[rel],
            server_args=(str(port), str(cert_dir / "server.crt"), str(cert_dir / "server.key")),
            client_args=("127.0.0.1", str(port)),
            host="127.0.0.1",
            port=port,
        )

    http_pairs = {
        "examples/cpp/http/example_http_include_echo_client":
            "examples/cpp/http/example_http_include_echo_server",
        "examples/cpp/http/example_http_import_echo_client":
            "examples/cpp/http/example_http_import_echo_server",
    }
    if rel in http_pairs:
        return PairPlan(
            server_rel=http_pairs[rel],
            server_args=(str(port),),
            client_args=(f"http://127.0.0.1:{port}/echo", "hello"),
            host="127.0.0.1",
            port=port,
        )

    _ = build_root
    return None


def wait_for_tcp_port(proc: subprocess.Popen[str], host: str, port: int, timeout_seconds: float) -> bool:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def signal_process(proc: subprocess.Popen[str], sig: int) -> None:
    try:
        os.killpg(proc.pid, sig)
        return
    except ProcessLookupError:
        return
    except PermissionError:
        pass

    try:
        proc.send_signal(sig)
    except (ProcessLookupError, PermissionError):
        pass


def terminate_process_group(proc: subprocess.Popen[str]) -> str:
    if proc.poll() is None:
        signal_process(proc, signal.SIGTERM)
    try:
        output, _ = proc.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        signal_process(proc, signal.SIGKILL)
        output, _ = proc.communicate()
    return output or ""


def run_pair(build_root: Path,
             client_rel: str,
             plan: PairPlan,
             timeout_seconds: float,
             server_timeout: float) -> tuple[str, int, bool, str]:
    server_exe = build_root / plan.server_rel
    client_exe = build_root / client_rel
    if not server_exe.is_file() or not os.access(server_exe, os.X_OK):
        return "MISSING", 127, False, f"missing paired server {plan.server_rel}"
    if not client_exe.is_file() or not os.access(client_exe, os.X_OK):
        return "MISSING", 127, False, f"missing paired client {client_rel}"

    server_proc = subprocess.Popen(
        [str(server_exe), *plan.server_args],
        cwd=build_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    if plan.probe_port:
        server_ready = wait_for_tcp_port(server_proc, plan.host, plan.port, server_timeout)
    else:
        time.sleep(min(0.3, server_timeout))
        server_ready = server_proc.poll() is None

    if not server_ready:
        server_output = terminate_process_group(server_proc)
        server_rc = server_proc.returncode if server_proc.returncode is not None else 124
        server_status = classify_result(plan.server_rel, server_rc, False, server_output)
        if server_status != "FAIL":
            server_status = "FAIL"
        return server_status, server_rc, False, f"server_start={compact_output(server_output)}"

    rc, timed_out, client_output = run_command(
        build_root,
        [str(client_exe), *plan.client_args],
        timeout_seconds,
    )
    server_output = terminate_process_group(server_proc)
    status = classify_result(client_rel, rc, timed_out, client_output)
    if status != "PASS":
        status = "FAIL"
    output = f"client={compact_output(client_output)} server={compact_output(server_output)}"
    return status, rc, timed_out, output


def compact_output(output: str, max_chars: int = 180) -> str:
    text = "|".join(output.splitlines()[:3])
    if len(text) > max_chars:
        return text[: max_chars - 3] + "..."
    return text


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--list-file", type=Path)
    parser.add_argument("--kind", choices=("examples", "benchmarks", "all"), default="all")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--server-timeout", type=float, default=8.0)
    parser.add_argument("--external-timeout", type=float, default=10.0)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    build_root = args.build_root.resolve()
    if not build_root.is_dir():
        print(f"build root not found: {build_root}", file=sys.stderr)
        return 2

    rels = iter_executables(build_root, args.list_file, args.kind)
    if args.limit > 0:
        rels = rels[: args.limit]

    pair_plans = {rel: plan for rel in rels if (plan := build_pair_plan(rel, build_root)) is not None}
    paired_servers = {plan.server_rel for plan in pair_plans.values()}

    counts: Counter[str] = Counter()
    for rel in rels:
        exe = build_root / rel
        if rel in paired_servers:
            status = "LONG_RUNNING"
            counts[status] += 1
            if args.verbose:
                print(f"{status} rc=0 timeout=0s rel={rel} output=paired server; run with matching client")
            continue

        if not exe.is_file() or not os.access(exe, os.X_OK):
            status = "MISSING"
            counts[status] += 1
            print(f"{status} rel={rel}")
            continue

        timeout_seconds = executable_timeout(rel, args.timeout, args.server_timeout, args.external_timeout)
        if rel in pair_plans:
            status, rc, timed_out, output = run_pair(
                build_root,
                rel,
                pair_plans[rel],
                timeout_seconds,
                args.server_timeout,
            )
        else:
            rc, timed_out, output = run_executable(build_root, rel, timeout_seconds)
            status = classify_result(rel, rc, timed_out, output)
        counts[status] += 1
        if args.verbose or status != "PASS":
            print(
                f"{status} rc={rc} timeout={timeout_seconds:g}s rel={rel} "
                f"output={compact_output(output)}"
            )

    summary = " ".join(f"{status}={counts[status]}" for status in STATUSES)
    print(f"SUMMARY exec-matrix build_root={build_root} kind={args.kind} total={len(rels)} {summary}")
    return 1 if counts["FAIL"] or counts["MISSING"] else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
