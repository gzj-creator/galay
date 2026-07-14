#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-release}"
RESULT_DIR="${RESULT_DIR:-${ROOT_DIR}/docs/cpp/modules/etcd/benchmark_data}"
RAW_DIR="${RAW_DIR:-${RESULT_DIR}/raw/full_module_2026-07-13}"
RESULT_CSV="${RESULT_CSV:-${RESULT_DIR}/etcd_client_compare_2026-07-13.csv}"
OFFICIAL_CSV="${OFFICIAL_CSV:-${RESULT_DIR}/etcdctl_check_perf_2026-07-13.csv}"
CLIENT_PORT="${CLIENT_PORT:-23790}"
PEER_PORT="${PEER_PORT:-23800}"
RUNS="${RUNS:-3}"
WORKERS="${WORKERS:-8}"
OPS_PER_WORKER="${OPS_PER_WORKER:-500}"
VALUE_SIZE="${VALUE_SIZE:-64}"
IO_SCHEDULERS="${IO_SCHEDULERS:-4}"
RUN_ETCDCTL_PERF="${RUN_ETCDCTL_PERF:-1}"

SYNC_BIN="${BUILD_DIR}/benchmark/cpp/etcd/benchmark_etcd_b1_kv"
ASYNC_BIN="${BUILD_DIR}/benchmark/cpp/etcd/benchmark_etcd_b2_kv"
ENDPOINT="http://127.0.0.1:${CLIENT_PORT}"
DATA_DIR="/tmp/galay-etcd-benchmark-${CLIENT_PORT}"
ETCD_PID=""
INVALID_CASES=0

cleanup() {
    if [[ -n "${ETCD_PID}" ]]; then
        kill "${ETCD_PID}" >/dev/null 2>&1 || true
        wait "${ETCD_PID}" >/dev/null 2>&1 || true
    fi
    if [[ -d "${DATA_DIR}" ]]; then
        rm -rf "${DATA_DIR}"
    fi
}
trap cleanup EXIT

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'missing command: %s\n' "$1" >&2
        exit 127
    fi
}

require_release_build() {
    local cache="${BUILD_DIR}/CMakeCache.txt"
    if [[ ! -f "${cache}" ]] || ! grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "${cache}"; then
        printf 'Release build required: %s\n' "${BUILD_DIR}" >&2
        exit 2
    fi
    if [[ ! -x "${SYNC_BIN}" || ! -x "${ASYNC_BIN}" ]]; then
        printf 'benchmark binary missing; build etcd benchmark targets first\n' >&2
        exit 2
    fi
}

wait_for_etcd() {
    local attempt
    for attempt in $(seq 1 50); do
        if etcdctl --endpoints="${ENDPOINT}" --command-timeout=200ms endpoint health >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

metric() {
    local file="$1"
    local name="$2"
    awk -F':' -v name="${name}" '
        {
            key = $1
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
            if (key == name) {
                value = $2
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
                print value
                exit
            }
        }
    ' "${file}"
}

run_client_case() {
    local implementation="$1"
    local mode="$2"
    local round="$3"
    local binary raw_file rc success failure throughput p50 p95 p99 valid
    if [[ "${implementation}" == "galay-sync" ]]; then
        binary="${SYNC_BIN}"
    else
        binary="${ASYNC_BIN}"
    fi
    raw_file="${RAW_DIR}/${implementation}_${mode}_run${round}.txt"

    set +e
    if [[ "${implementation}" == "galay-sync" ]]; then
        "${binary}" "${ENDPOINT}" "${WORKERS}" "${OPS_PER_WORKER}" "${VALUE_SIZE}" "${mode}" \
            >"${raw_file}" 2>&1
    else
        "${binary}" "${ENDPOINT}" "${WORKERS}" "${OPS_PER_WORKER}" "${VALUE_SIZE}" "${mode}" \
            "${IO_SCHEDULERS}" >"${raw_file}" 2>&1
    fi
    rc=$?
    set -e

    success="$(metric "${raw_file}" Success)"
    failure="$(metric "${raw_file}" Failure)"
    throughput="$(metric "${raw_file}" Throughput | awk '{print $1}')"
    p50="$(metric "${raw_file}" 'Latency p50' | awk '{print $1}')"
    p95="$(metric "${raw_file}" 'Latency p95' | awk '{print $1}')"
    p99="$(metric "${raw_file}" 'Latency p99' | awk '{print $1}')"
    valid=false
    if [[ "${rc}" -eq 0 && "${success:-0}" -gt 0 && "${failure:-1}" -eq 0 ]] && \
       awk -v throughput="${throughput:-0}" 'BEGIN { exit !(throughput > 0) }'; then
        valid=true
    else
        INVALID_CASES=$((INVALID_CASES + 1))
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${implementation}" "${mode}" "${round}" "${WORKERS}" "${OPS_PER_WORKER}" \
        "${VALUE_SIZE}" "${IO_SCHEDULERS}" "${success:-0}" "${failure:-0}" "${throughput:-0}" \
        "${p50:-0}" "${p95:-0}" "${p99:-0}" "${valid}" "${raw_file#${ROOT_DIR}/}" \
        >>"${RESULT_CSV}"
}

run_etcdctl_case() {
    local round="$1"
    local raw_file rc throughput valid
    raw_file="${RAW_DIR}/etcdctl_check_perf_s_run${round}.txt"
    set +e
    etcdctl --endpoints="${ENDPOINT}" check perf --load=s \
        --prefix="/galay-etcdctl-check-perf/run${round}/" >"${raw_file}" 2>&1
    rc=$?
    set -e
    throughput="$(awk '/Throughput is/ { print $(NF - 1); exit }' "${raw_file}")"
    valid=false
    if [[ "${rc}" -eq 0 ]] && awk -v throughput="${throughput:-0}" \
        'BEGIN { exit !(throughput > 0) }'; then
        valid=true
    else
        INVALID_CASES=$((INVALID_CASES + 1))
    fi
    printf '%s,%s,%s,%s,%s\n' \
        "${round}" s "${throughput:-0}" "${valid}" "${raw_file#${ROOT_DIR}/}" \
        >>"${OFFICIAL_CSV}"
}

require_cmd etcd
require_cmd etcdctl
require_release_build
mkdir -p "${RAW_DIR}"

if etcdctl --endpoints="${ENDPOINT}" --command-timeout=1s endpoint health >/dev/null 2>&1; then
    printf 'refusing to benchmark an existing etcd on %s\n' "${ENDPOINT}" >&2
    exit 2
fi

rm -rf "${DATA_DIR}"
etcd --name galay-benchmark --data-dir "${DATA_DIR}" \
    --listen-client-urls "${ENDPOINT}" --advertise-client-urls "${ENDPOINT}" \
    --listen-peer-urls "http://127.0.0.1:${PEER_PORT}" \
    --initial-advertise-peer-urls "http://127.0.0.1:${PEER_PORT}" \
    --initial-cluster "galay-benchmark=http://127.0.0.1:${PEER_PORT}" \
    --initial-cluster-state new --logger zap --log-level error \
    >"${RAW_DIR}/etcd_server.log" 2>&1 &
ETCD_PID=$!
if ! wait_for_etcd; then
    printf 'isolated etcd failed to start\n' >&2
    exit 1
fi

printf 'implementation,mode,round,workers,ops_per_worker,value_size,io_schedulers,success,failure,throughput_ops_sec,p50_us,p95_us,p99_us,valid,raw_file\n' >"${RESULT_CSV}"
printf 'round,load,throughput_writes_sec,valid,raw_file\n' >"${OFFICIAL_CSV}"

for round in $(seq 1 "${RUNS}"); do
    run_client_case galay-sync put "${round}"
    run_client_case galay-sync mixed "${round}"
    run_client_case galay-async put "${round}"
    run_client_case galay-async mixed "${round}"
    if [[ "${RUN_ETCDCTL_PERF}" == "1" ]]; then
        run_etcdctl_case "${round}"
    fi
done

printf 'etcd client CSV: %s\n' "${RESULT_CSV}"
printf 'etcdctl check perf CSV: %s\n' "${OFFICIAL_CSV}"
if [[ "${INVALID_CASES}" -ne 0 ]]; then
    printf 'invalid cases: %s\n' "${INVALID_CASES}" >&2
    exit 1
fi
