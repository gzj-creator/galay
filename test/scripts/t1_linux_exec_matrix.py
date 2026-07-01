#!/usr/bin/env python3
"""Tests for Linux example/benchmark execution-result classification."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "verify_linux_exec_matrix.py"


def load_module():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("verify_linux_exec_matrix", SCRIPT)
    if spec is None or spec.loader is None:
        raise AssertionError("unable to load verify_linux_exec_matrix.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    matrix = load_module()

    check(
        matrix.classify_result("benchmark/cpp/utils/benchmark_utils_consistent_hash_lookup", 0, False, "")
        == "PASS",
        "rc=0 should be PASS",
    )
    check(
        matrix.classify_result("benchmark/cpp/ssl/benchmark_ssl_tls_steady_state", 0, False, "[SKIP] certs")
        == "SKIP",
        "explicit skip output should beat rc=0",
    )
    check(
        matrix.classify_result("benchmark/cpp/rpc/benchmark_rpc_etcd_managed_client_pressure", 125, False, "[SKIP]")
        == "SKIP",
        "rc=125 should be SKIP",
    )
    check(
        matrix.classify_result(
            "benchmark/cpp/http/benchmark_http_echo_server_throughput",
            124,
            True,
            "server listening",
        )
        == "LONG_RUNNING",
        "server timeout should be LONG_RUNNING",
    )
    check(
        matrix.classify_result(
            "benchmark/cpp/http/benchmark_http_echo_server_throughput",
            1,
            False,
            "bind failed",
        )
        == "FAIL",
        "non-timeout server failure should be FAIL",
    )
    check(
        matrix.classify_result(
            "benchmark/cpp/mysql/benchmark_mysql_sync_query_pressure",
            1,
            False,
            "connect failed: connection refused",
        )
        == "EXTERNAL_DEP",
        "mysql benchmark without DB should be EXTERNAL_DEP",
    )
    check(
        matrix.classify_result(
            "examples/cpp/etcd/example_etcd_include_async_basic",
            -11,
            False,
            "connect failed: connection refused",
        )
        == "FAIL",
        "signal exits must not be hidden as external dependencies",
    )
    check(
        matrix.classify_result(
            "benchmark/c/kernel/benchmark_c_kernel_tcp_socket_client_throughput",
            10,
            False,
            "[EXTERNAL_DEP] TCP echo server is required",
        )
        == "EXTERNAL_DEP",
        "explicit external dependency marker should be EXTERNAL_DEP",
    )
    check(
        matrix.classify_result(
            "benchmark/c/kernel/benchmark_c_kernel_tcp_socket_client_throughput",
            10,
            False,
            "tcp_socket_client_throughput errors=32",
        )
        == "NEEDS_PEER",
        "standalone client benchmark should be NEEDS_PEER",
    )
    check(
        matrix.classify_result(
            "benchmark/cpp/ssl/benchmark_ssl_tls_server_throughput",
            124,
            True,
            "LONG_RUNNING: stop with SIGINT",
        )
        == "LONG_RUNNING",
        "explicit long-running marker should be LONG_RUNNING",
    )
    check(
        matrix.classify_result(
            "benchmark/cpp/ssl/benchmark_ssl_tls_server_throughput",
            1,
            False,
            "LONG_RUNNING: stop with SIGINT",
        )
        == "FAIL",
        "LONG_RUNNING usage text must not hide immediate non-timeout failure",
    )
    check(
        matrix.classify_result("benchmark/cpp/utils/benchmark_utils_unknown", 1, False, "unexpected")
        == "FAIL",
        "unknown non-zero status should be FAIL",
    )
    check(
        matrix.classify_result(
            "examples/c/kernel/example_c_kernel_async_file_copy",
            13,
            False,
            "async_file_copy unsupported: operation unsupported",
        )
        == "SKIP",
        "explicit unsupported backend should be SKIP",
    )
    check(
        matrix.classify_result(
            "examples/cpp/http/example_http_manual_t10_chunk",
            1,
            False,
            "manual scenario requires paired setup",
        )
        == "NEEDS_PEER",
        "manual examples should be NEEDS_PEER when standalone execution fails",
    )

    pair = matrix.build_pair_plan(
        "benchmark/c/kernel/benchmark_c_kernel_tcp_socket_client_throughput",
        ROOT / "build-local-full",
    )
    check(pair is not None, "known TCP throughput client should have a pair plan")
    check(
        pair.server_rel == "benchmark/c/kernel/benchmark_c_kernel_tcp_socket_server_throughput",
        "TCP throughput pair should launch the matching server",
    )
    check("-p" in pair.client_args, "TCP throughput pair should pass the dynamic server port")
    check(
        matrix.single_executable_args("benchmark/c/kernel/benchmark_c_kernel_async_waiter_signal") == ("100",),
        "async waiter signal benchmark should run a short smoke workload",
    )
    check(
        matrix.single_executable_args("benchmark/c/kernel/benchmark_c_kernel_coro_tcp_iov_sendfile") == ("20",),
        "sendfile benchmark should run a short smoke workload",
    )
    ssl_pair = matrix.build_pair_plan(
        "examples/cpp/ssl/example_ssl_include_echo_client",
        ROOT / "build-local-full",
    )
    check(ssl_pair is not None, "known SSL echo client should have a pair plan")
    check(
        ssl_pair.server_rel == "examples/cpp/ssl/example_ssl_include_echo_server",
        "SSL echo pair should launch the matching server before the client",
    )
    original_killpg = matrix.os.killpg

    class FakeProc:
        pid = 12345

        def __init__(self):
            self.signals = []

        def send_signal(self, sig):
            self.signals.append(sig)

    def deny_killpg(pid, sig):
        raise PermissionError()

    fake_proc = FakeProc()
    matrix.os.killpg = deny_killpg
    try:
        matrix.signal_process(fake_proc, matrix.signal.SIGTERM)
    finally:
        matrix.os.killpg = original_killpg
    check(
        fake_proc.signals == [matrix.signal.SIGTERM],
        "process cleanup should fall back when killpg is denied",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
