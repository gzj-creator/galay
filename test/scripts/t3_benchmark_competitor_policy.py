#!/usr/bin/env python3
"""Lock the formal benchmark policy and its checked-in Asio evidence."""

from __future__ import annotations

import csv
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ASIO_SOURCE = ROOT / "benchmark/cpp/kernel/compare/boost-asio-coro/boost_asio_coro_udp.cc"
ASIO_TCP_SOURCE = ROOT / "benchmark/cpp/kernel/compare/boost-asio-coro/boost_asio_coro_tcp.cc"
CSV_PATH = (
    ROOT
    / "docs/cpp/modules/kernel/benchmark_data/kernel_boost_asio_coro_udp_2026-08-21.csv"
)
TCP_CSV_PATH = (
    ROOT
    / "docs/cpp/modules/kernel/benchmark_data/kernel_boost_asio_coro_tcp_2026-08-21.csv"
)
FORMAL_RAW_DIR = ROOT / "docs/cpp/modules/kernel/benchmark_data/raw"
KERNEL_DOC = ROOT / "docs/cpp/modules/kernel/05-性能测试.md"
TENCENT_RUNNER = ROOT / "scripts/tencent_full_test.sh"

POLICY_SCRIPTS = (
    "scripts/etcd/300_etcd_compare_etcdctl.sh",
    "scripts/http2/300_http2_h2load_compare.sh",
    "scripts/mysql/300_mysql_compare_libmysqlclient.sh",
    "scripts/redis/300_redis_compare_hiredis.sh",
    "scripts/rpc/302_rpc_compare_open_source.sh",
)

PERFORMANCE_DOCS = tuple(ROOT.glob("docs/**/05-性能测试.md"))

POLICY_NOTICE = "正式对标政策（2026-08-21）"

FORBIDDEN_TARGETS = {
    "benchmark/c/kernel/CMakeLists.txt": ("benchmark_c_kernel_libuv_echo_server PRIVATE",),
    "benchmark/cpp/kernel/CMakeLists.txt": (
        "GALAY_KERNEL_COMPARE_SOURCES",
        "benchmark_kernel_compare_mpsc_paired",
        "benchmark_kernel_compare_spsc_paired",
    ),
    "benchmark/cpp/mysql/CMakeLists.txt": ("benchmark_mysql_libmysqlclient_query_pressure",),
    "benchmark/cpp/postgres/CMakeLists.txt": ("benchmark_postgres_libpq_",),
    "benchmark/cpp/redis/CMakeLists.txt": ("benchmark_redis_hiredis_client_throughput",),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_key_values(line: str) -> dict[str, str]:
    return dict(part.split("=", 1) for part in line.split() if "=" in part)


def close(actual: str, expected: str, tolerance: float = 0.02) -> bool:
    return abs(float(actual) - float(expected)) <= tolerance


def resolve_formal_raw(raw_file: str, label: str) -> Path:
    raw_path = (ROOT / raw_file).resolve()
    require(
        raw_path.parent == FORMAL_RAW_DIR,
        f"{label} raw output must be checked in under {FORMAL_RAW_DIR.relative_to(ROOT)}: {raw_file}",
    )
    require(raw_path.is_file(), f"missing {label} raw output: {raw_file}")
    return raw_path


def verify_policy() -> None:
    kernel_cmake = (ROOT / "benchmark/cpp/kernel/CMakeLists.txt").read_text()
    require(
        "benchmark_kernel_compare_boost_asio_coro_udp" in kernel_cmake,
        "Boost.Asio coroutine target is not registered",
    )
    require(
        "benchmark_kernel_compare_boost_asio_coro_tcp" in kernel_cmake,
        "Boost.Asio TCP coroutine target is not registered",
    )

    asio_source = ASIO_SOURCE.read_text()
    for needle in ("co_spawn", "awaitable", "async_send_to", "async_receive_from"):
        require(needle in asio_source, f"Asio coroutine baseline is missing {needle}")
    require("async_connect" not in asio_source, "connected UDP fast path is not equivalent to Galay")

    asio_tcp_source = ASIO_TCP_SOURCE.read_text()
    for needle in ("co_spawn", "awaitable", "async_connect", "async_read", "async_write"):
        require(needle in asio_tcp_source, f"Asio TCP coroutine baseline is missing {needle}")
    galay_tcp_source = (ROOT / "benchmark/cpp/kernel/b31_tcp_socket_fair_throughput.cc").read_text()
    for needle in ("enum class Phase", "readExact", "writeAll", "settledCountersMatch"):
        require(needle in galay_tcp_source, f"Galay TCP fair harness is missing {needle}")

    runner = TENCENT_RUNNER.read_text()
    require(
        'BENCHMARK_CPU="${BENCHMARK_CPU:-0}"' in runner,
        "formal runner must expose one shared benchmark CPU",
    )
    require(
        'taskset -c "${BENCHMARK_CPU}" "${galay_bin}"' in runner,
        "formal runner must pin Galay to the benchmark CPU",
    )
    require(
        'taskset -c "${BENCHMARK_CPU}" "${asio_bin}"' in runner,
        "formal runner must pin Boost.Asio to the benchmark CPU",
    )
    require(
        'FORMAL_RUNS="${FORMAL_RUNS:-3}"' in runner,
        "formal runner must use the three-round evidence default",
    )
    require(
        "boost_asio_coro.csv" in runner,
        "formal runner must emit a machine-readable Asio comparison CSV",
    )
    require(
        "boost_asio_coro_raw" in runner,
        "formal runner must preserve one raw file per implementation and round",
    )
    require(
        "run_boost_asio_tcp_comparison" in runner,
        "formal runner must execute the TCP comparison",
    )
    require(
        "boost_asio_coro_tcp.csv" in runner,
        "formal runner must emit a TCP comparison CSV",
    )
    require(
        "boost_asio_coro_tcp_raw" in runner,
        "formal runner must preserve TCP raw output",
    )

    for relative, forbidden in FORBIDDEN_TARGETS.items():
        source = (ROOT / relative).read_text()
        for needle in forbidden:
            require(needle not in source, f"historical competitor target remains registered: {needle}")

    for relative in POLICY_SCRIPTS:
        completed = subprocess.run(
            ["bash", str(ROOT / relative)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(completed.returncode == 0, f"policy script failed: {relative}")
        values = parse_key_values(completed.stdout)
        require(values.get("status") == "not_applicable", f"invalid status from {relative}")
        require(
            "competitor=boost.asio coroutine" in completed.stdout.splitlines(),
            f"invalid formal competitor from {relative}",
        )

    require(PERFORMANCE_DOCS, "no module performance documentation found")
    for doc_path in PERFORMANCE_DOCS:
        doc = doc_path.read_text()
        require(
            POLICY_NOTICE in doc[:1000],
            f"performance page is missing the current formal policy: {doc_path.relative_to(ROOT)}",
        )


def parse_galay_raw(raw: str) -> dict[str, str]:
    machine_lines = raw.splitlines()
    machine_meta = next((line for line in machine_lines if line.startswith("meta ")), None)
    machine_measured = next(
        (line for line in machine_lines if line.startswith("measured ")), None
    )
    machine_settled = next(
        (line for line in machine_lines if line.startswith("settled ")), None
    )
    machine_status = next((line for line in machine_lines if line.startswith("status=")), None)
    if machine_meta is not None and machine_measured is not None and machine_settled is not None:
        meta = parse_key_values(machine_meta)
        measured = parse_key_values(machine_measured)
        settled = parse_key_values(machine_settled)
        require(meta.get("ready_clients") == "100", "Galay clients were not all ready")
        require(machine_status == "status=ok", "Galay benchmark did not pass")
        require(measured.get("runtime_errors") == "0", "Galay measured runtime errors")
        require(measured.get("shutdown_errors") == "0", "Galay measured shutdown errors")
        require(settled.get("runtime_errors") == "0", "Galay settled runtime errors")
        require(settled.get("shutdown_errors") == "0", "Galay settled shutdown errors")
        return {
            **measured,
            "settled_client_sent": settled["client_sent"],
            "settled_client_received": settled["client_received"],
            "settled_server_received": settled["server_received"],
            "settled_server_sent": settled["server_sent"],
            "settled_client_loss_pct": settled["client_loss_pct"],
        }

    # Keep the legacy formatter parser for already archived diagnostic output.
    client = re.search(
        r"Measurement window client:.*\) (\d+) ([0-9.]+) [0-9.]+ (\d+) ([0-9.]+)",
        raw,
    )
    server = re.search(
        r"Measurement window server:.*\) (\d+) ([0-9.]+) [0-9.]+ (\d+) ([0-9.]+)",
        raw,
    )
    errors = re.search(r"Errors: \{\} (\d+)", raw)
    ready = re.search(r"Ready Clients: \{\} (\d+)", raw)
    settled = re.search(
        r"Settled measured packets: client_sent=(\d+) client_received=(\d+) "
        r"server_received=(\d+) server_sent=(\d+) loss=([0-9.]+)%",
        raw,
    )
    require(
        client is not None and server is not None and errors is not None and ready is not None
        and settled is not None,
        "invalid Galay raw output",
    )
    require(ready.group(1) == "100", "Galay clients were not all ready")
    sent, client_rate, received, _ = client.groups()
    server_received, server_rate, server_sent, _ = server.groups()
    settled_sent, settled_received, settled_server_received, settled_server_sent, settled_loss = (
        settled.groups()
    )
    measured_loss = (1.0 - int(received) / int(sent)) * 100.0 if int(sent) else 100.0
    return {
        "client_sent": sent,
        "client_received": received,
        "server_received": server_received,
        "server_sent": server_sent,
        "settled_client_sent": settled_sent,
        "settled_client_received": settled_received,
        "settled_server_received": settled_server_received,
        "settled_server_sent": settled_server_sent,
        "client_pkt_s": client_rate,
        "server_pkt_s": server_rate,
        "client_loss_pct": str(measured_loss),
        "settled_client_loss_pct": settled_loss,
        "runtime_errors": errors.group(1),
    }


def parse_asio_raw(raw: str) -> dict[str, str]:
    lines = raw.splitlines()
    require(len(lines) == 4, "invalid Boost.Asio raw output line count")
    meta = parse_key_values(lines[0])
    measured = parse_key_values(lines[1])
    settled = parse_key_values(lines[2])
    require(meta.get("ready_clients") == "100", "Boost.Asio clients were not all ready")
    require(measured.get("runtime_errors") == "0", "Boost.Asio measured runtime errors")
    require(measured.get("shutdown_errors") == "0", "Boost.Asio measured shutdown errors")
    require(settled.get("runtime_errors") == "0", "Boost.Asio settled runtime errors")
    require(settled.get("shutdown_errors") == "0", "Boost.Asio settled shutdown errors")
    require(lines[3] == "status=ok", "Boost.Asio benchmark did not pass")
    return {
        **measured,
        "settled_client_sent": settled["client_sent"],
        "settled_client_received": settled["client_received"],
        "settled_server_received": settled["server_received"],
        "settled_server_sent": settled["server_sent"],
        "settled_client_loss_pct": settled["client_loss_pct"],
    }


def verify_evidence() -> None:
    with CSV_PATH.open(newline="") as source:
        rows = list(csv.DictReader(source))
    require(len(rows) == 6, "formal CSV must contain three alternating pairs")
    require(
        [(row["implementation"], row["run"]) for row in rows]
        == [(implementation, str(run)) for run in range(1, 4) for implementation in ("galay", "boost.asio")],
        "formal CSV must use Galay/Asio alternating order for each run",
    )

    exact_fields = (
        "client_sent",
        "client_received",
        "server_received",
        "server_sent",
        "settled_client_sent",
        "settled_client_received",
        "settled_server_received",
        "settled_server_sent",
        "runtime_errors",
    )
    rate_fields = (
        "client_pkt_s",
        "server_pkt_s",
        "client_loss_pct",
        "settled_client_loss_pct",
    )
    fixed_config = {
        "scenario": "udp-echo",
        "clients": "100",
        "workers": "4",
        "payload_bytes": "256",
        "pipeline": "1",
        "warmup_s": "1",
        "duration_s": "5",
    }
    for row in rows:
        require(row["status"] == "ok", "CSV contains a failed sample")
        require(row["runtime_errors"] == "0", "CSV contains runtime errors")
        require(row["shutdown_errors"] == "0", "CSV contains measured shutdown errors")
        for field, expected in fixed_config.items():
            require(row[field] == expected, f"CSV uses a non-equivalent {field}")
        raw_path = resolve_formal_raw(row["raw_file"], "UDP")
        raw = raw_path.read_text()
        parsed = parse_galay_raw(raw) if row["implementation"] == "galay" else parse_asio_raw(raw)
        for field in exact_fields:
            require(row[field] == parsed[field], f"CSV/raw mismatch for {field}: {raw_path.name}")
        for field in rate_fields:
            require(close(row[field], parsed[field]), f"CSV/raw mismatch for {field}: {raw_path.name}")

    doc = KERNEL_DOC.read_text()
    for expected in ("12,373.6", "7,548.14", "1.639x", "settled client loss"):
        require(expected in doc, f"kernel performance summary is missing {expected}")
    for stale in (
        "12,421.3",
        "7,436.43",
        "1.670x",
        "5.26 个百分点",
        "12,184.4",
        "7,425.06",
        "1.641x",
        "6.08 个百分点",
        "12,251.2",
        "7,381.60",
        "1.660x",
        "12,405.0",
        "8,625.6",
        "1.438x",
    ):
        require(stale not in doc, f"kernel performance summary still contains stale value {stale}")


def verify_tcp_evidence() -> None:
    with TCP_CSV_PATH.open(newline="") as source:
        rows = list(csv.DictReader(source))
    require(len(rows) == 6, "formal TCP CSV must contain three alternating pairs")
    require(
        [(row["implementation"], row["run"]) for row in rows]
        == [
            (implementation, str(run))
            for run in range(1, 4)
            for implementation in ("galay", "boost.asio")
        ],
        "formal TCP CSV must use Galay/Asio alternating order for each run",
    )

    fixed_config = {
        "scenario": "tcp-echo",
        "clients": "100",
        "workers": "4",
        "payload_bytes": "256",
        "pipeline": "1",
        "warmup_s": "1",
        "duration_s": "5",
    }
    exact_fields = (
        "client_sent",
        "client_received",
        "server_received",
        "server_sent",
        "settled_client_sent",
        "settled_client_received",
        "settled_server_received",
        "settled_server_sent",
        "runtime_errors",
    )
    rate_fields = (
        "client_pkt_s",
        "server_pkt_s",
        "client_loss_pct",
        "settled_client_loss_pct",
    )
    for row in rows:
        require(row["status"] == "ok", "TCP CSV contains a failed sample")
        require(row["runtime_errors"] == "0", "TCP CSV contains runtime errors")
        require(row["shutdown_errors"] == "0", "TCP CSV contains shutdown errors")
        for field, expected in fixed_config.items():
            require(row[field] == expected, f"TCP CSV uses a non-equivalent {field}")
        raw_path = resolve_formal_raw(row["raw_file"], "TCP")
        raw = raw_path.read_text()
        parsed = parse_galay_raw(raw) if row["implementation"] == "galay" else parse_asio_raw(raw)
        for field in exact_fields:
            require(row[field] == parsed[field], f"TCP CSV/raw mismatch for {field}: {raw_path.name}")
        for field in rate_fields:
            require(close(row[field], parsed[field]), f"TCP CSV/raw mismatch for {field}: {raw_path.name}")

    doc = KERNEL_DOC.read_text()
    for expected in ("Boost.Asio 协程 TCP 公平对标", "tcp-echo", "TCP settled"):
        require(expected in doc, f"kernel performance summary is missing TCP evidence marker {expected}")
    for expected in ("4,307.61", "5,927.91", "0.727x"):
        require(expected in doc, f"kernel TCP performance summary is missing {expected}")


def main() -> int:
    verify_policy()
    verify_evidence()
    verify_tcp_evidence()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
