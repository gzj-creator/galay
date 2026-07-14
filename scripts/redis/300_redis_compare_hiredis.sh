#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-release}"
RESULT_DIR="${RESULT_DIR:-${ROOT_DIR}/docs/cpp/modules/redis/benchmark_data}"
RAW_DIR="${RAW_DIR:-${RESULT_DIR}/raw/full_module_2026-07-13}"
RESULT_CSV="${RESULT_CSV:-${RESULT_DIR}/redis_hiredis_compare_2026-07-13.csv}"
OFFICIAL_CSV="${OFFICIAL_CSV:-${RESULT_DIR}/redis_official_compare_2026-07-13.csv}"
POOL_CSV="${POOL_CSV:-${RESULT_DIR}/redis_pool_compare_2026-07-13.csv}"
REDIS_PORT="${REDIS_PORT:-6398}"
CLIENTS="${CLIENTS:-10}"
OPERATIONS="${OPERATIONS:-5000}"
RUNS="${RUNS:-3}"
BATCH_SIZES="${BATCH_SIZES:-10 50 100}"
BUFFER_SIZE="${BUFFER_SIZE:-65536}"

GALAY_BIN="${BUILD_DIR}/benchmark/cpp/redis/benchmark_redis_async_client_throughput"
HIREDIS_BIN="${BUILD_DIR}/benchmark/cpp/redis/benchmark_redis_hiredis_client_throughput"
POOL_BIN="${BUILD_DIR}/benchmark/cpp/redis/benchmark_redis_connection_pool_throughput"
REDIS_PID=""
INVALID_CASES=0

cleanup() {
    if [[ -n "${REDIS_PID}" ]]; then
        if ! redis-cli -h 127.0.0.1 -p "${REDIS_PORT}" shutdown nosave >/dev/null 2>&1; then
            kill "${REDIS_PID}" >/dev/null 2>&1 || true
        fi
        wait "${REDIS_PID}" >/dev/null 2>&1 || true
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
    if [[ ! -x "${GALAY_BIN}" || ! -x "${HIREDIS_BIN}" || ! -x "${POOL_BIN}" ]]; then
        printf 'benchmark binary missing; build Redis benchmark targets first\n' >&2
        exit 2
    fi
}

run_pool_case() {
    local round="$1"
    local raw_file rc success error timeout ops valid
    raw_file="${RAW_DIR}/galay_pool_run${round}.txt"
    set +e
    "${POOL_BIN}" -h 127.0.0.1 -p "${REDIS_PORT}" -c "${CLIENTS}" -n "${OPERATIONS}" \
        -m "${CLIENTS}" -x "${CLIENTS}" -q >"${raw_file}" 2>&1
    rc=$?
    set -e
    success="$(metric "${raw_file}" Success)"
    error="$(metric "${raw_file}" Error)"
    timeout="$(metric "${raw_file}" Timeout)"
    ops="$(metric "${raw_file}" 'Ops/sec')"
    valid=false
    if [[ "${rc}" -eq 0 && "${success:-0}" -gt 0 && "${error:-1}" -eq 0 && \
          "${timeout:-1}" -eq 0 && "${ops:-0}" -gt 0 ]]; then
        valid=true
    else
        INVALID_CASES=$((INVALID_CASES + 1))
    fi
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${round}" "${CLIENTS}" "${OPERATIONS}" "${CLIENTS}" "${CLIENTS}" \
        "${success:-0}" "${error:-0}" "${timeout:-0}" "${ops:-0}" "${valid}" \
        "${raw_file#${ROOT_DIR}/}" >>"${POOL_CSV}"
}

wait_for_redis() {
    local attempt
    for attempt in 1 2 3 4 5 6 7 8 9 10; do
        if redis-cli -h 127.0.0.1 -p "${REDIS_PORT}" ping >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

metric() {
    local file="$1"
    local name="$2"
    awk -F': ' -v name="${name}" '$1 == name { print $2; exit }' "${file}"
}

run_client_case() {
    local implementation="$1"
    local mode="$2"
    local batch_size="$3"
    local round="$4"
    local binary label raw_file rc success error timeout ops p50 p99 valid

    if [[ "${implementation}" == "galay" ]]; then
        binary="${GALAY_BIN}"
    else
        binary="${HIREDIS_BIN}"
    fi
    label="${implementation}_${mode}_b${batch_size}_run${round}"
    raw_file="${RAW_DIR}/${label}.txt"

    set +e
    if [[ "${implementation}" == "galay" ]]; then
        "${binary}" -h 127.0.0.1 -p "${REDIS_PORT}" -c "${CLIENTS}" -n "${OPERATIONS}" \
            -m "${mode}" -b "${batch_size}" --timeout-ms -1 --buffer-size "${BUFFER_SIZE}" -q \
            >"${raw_file}" 2>&1
    else
        "${binary}" -h 127.0.0.1 -p "${REDIS_PORT}" -c "${CLIENTS}" -n "${OPERATIONS}" \
            -m "${mode}" -b "${batch_size}" -q >"${raw_file}" 2>&1
    fi
    rc=$?
    set -e

    success="$(metric "${raw_file}" Success)"
    error="$(metric "${raw_file}" Error)"
    timeout="$(metric "${raw_file}" Timeout)"
    ops="$(metric "${raw_file}" 'Ops/sec')"
    p50="$(metric "${raw_file}" 'Request latency p50 (us)')"
    p99="$(metric "${raw_file}" 'Request latency p99 (us)')"
    valid=false
    if [[ "${rc}" -eq 0 && "${success:-0}" -gt 0 && "${error:-1}" -eq 0 && \
          "${timeout:-1}" -eq 0 && "${ops:-0}" -gt 0 ]]; then
        valid=true
    else
        INVALID_CASES=$((INVALID_CASES + 1))
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${implementation}" "${mode}" "${batch_size}" "${round}" "${CLIENTS}" \
        "${OPERATIONS}" "${success:-0}" "${error:-0}" "${timeout:-0}" "${ops:-0}" \
        "${p50:-0}" "${p99:-0}" "${valid}" "${raw_file#${ROOT_DIR}/}" >>"${RESULT_CSV}"
}

run_official_case() {
    local pipeline="$1"
    local round="$2"
    local total_requests raw_file rc valid
    total_requests=$((CLIENTS * OPERATIONS))
    raw_file="${RAW_DIR}/redis_benchmark_p${pipeline}_run${round}.csv"

    set +e
    redis-benchmark -h 127.0.0.1 -p "${REDIS_PORT}" -c "${CLIENTS}" \
        -n "${total_requests}" -d 8 -P "${pipeline}" -t set,get,incr --csv \
        >"${raw_file}" 2>&1
    rc=$?
    set -e
    valid=false
    if [[ "${rc}" -eq 0 ]]; then
        valid=true
    else
        INVALID_CASES=$((INVALID_CASES + 1))
    fi
    awk -F',' -v pipeline="${pipeline}" -v round="${round}" \
        -v requests="${total_requests}" -v valid="${valid}" \
        -v raw_file="${raw_file#${ROOT_DIR}/}" '
        NR > 1 {
            for (i = 1; i <= NF; ++i) gsub(/"/, "", $i)
            printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                pipeline, round, requests, $1, $2, $3, $4, $5, $6, $7, $8,
                valid, raw_file, "official-reference-only"
        }
    ' "${raw_file}" >>"${OFFICIAL_CSV}"
}

require_cmd redis-server
require_cmd redis-cli
require_cmd redis-benchmark
require_release_build
mkdir -p "${RAW_DIR}"

if redis-cli -h 127.0.0.1 -p "${REDIS_PORT}" ping >/dev/null 2>&1; then
    printf 'refusing to benchmark an existing Redis on port %s\n' "${REDIS_PORT}" >&2
    exit 2
fi

redis-server --bind 127.0.0.1 --port "${REDIS_PORT}" --save '' --appendonly no \
    --daemonize no --dir /tmp >"${RAW_DIR}/redis_server.log" 2>&1 &
REDIS_PID=$!
if ! wait_for_redis; then
    printf 'isolated Redis failed to start\n' >&2
    exit 1
fi

printf 'implementation,mode,batch_size,round,clients,operations_per_client,success,error,timeout,ops_sec,p50_us,p99_us,valid,raw_file\n' >"${RESULT_CSV}"
printf 'pipeline,round,total_requests,test,rps,avg_latency_ms,min_latency_ms,p50_latency_ms,p95_latency_ms,p99_latency_ms,max_latency_ms,valid,raw_file,classification\n' >"${OFFICIAL_CSV}"
printf 'round,workers,operations_per_worker,pool_min,pool_max,success,error,timeout,ops_sec,valid,raw_file\n' >"${POOL_CSV}"

for round in $(seq 1 "${RUNS}"); do
    run_client_case galay normal 1 "${round}"
    run_client_case hiredis normal 1 "${round}"
    run_pool_case "${round}"
    run_official_case 1 "${round}"
    for batch_size in ${BATCH_SIZES}; do
        run_client_case galay pipeline "${batch_size}" "${round}"
        run_client_case hiredis pipeline "${batch_size}" "${round}"
        run_official_case "${batch_size}" "${round}"
    done
done

printf 'Redis comparison CSV: %s\n' "${RESULT_CSV}"
printf 'Redis official CSV index: %s\n' "${OFFICIAL_CSV}"
printf 'Redis pool CSV: %s\n' "${POOL_CSV}"
if [[ "${INVALID_CASES}" -ne 0 ]]; then
    printf 'invalid cases: %s\n' "${INVALID_CASES}" >&2
    exit 1
fi
