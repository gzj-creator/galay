#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-release}"
RESULT_DIR="${RESULT_DIR:-${ROOT_DIR}/docs/cpp/modules/mysql/benchmark_data}"
RAW_DIR="${RAW_DIR:-${RESULT_DIR}/raw/full_module_2026-07-13}"
RESULT_CSV="${RESULT_CSV:-${RESULT_DIR}/mysql_libmysqlclient_compare_2026-07-13.csv}"
MYSQLSLAP_CSV="${MYSQLSLAP_CSV:-${RESULT_DIR}/mysqlslap_compare_2026-07-13.csv}"
RUNS="${RUNS:-3}"
CLIENTS="${CLIENTS:-8}"
QUERIES="${QUERIES:-1000}"
WARMUP="${WARMUP:-100}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-180}"
SQL="${SQL:-SELECT 1}"

export GALAY_MYSQL_HOST="${GALAY_MYSQL_HOST:-127.0.0.1}"
export GALAY_MYSQL_PORT="${GALAY_MYSQL_PORT:-3306}"
export GALAY_MYSQL_USER="${GALAY_MYSQL_USER:-root}"
export GALAY_MYSQL_PASSWORD="${GALAY_MYSQL_PASSWORD-}"
export GALAY_MYSQL_DB="${GALAY_MYSQL_DB:-test}"

GALAY_BIN="${BUILD_DIR}/benchmark/cpp/mysql/benchmark_mysql_sync_query_pressure"
MYSQLCLIENT_BIN="${BUILD_DIR}/benchmark/cpp/mysql/benchmark_mysql_libmysqlclient_query_pressure"
INVALID_CASES=0

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
    if [[ ! -x "${GALAY_BIN}" || ! -x "${MYSQLCLIENT_BIN}" ]]; then
        printf 'benchmark binary missing; build MySQL benchmark targets first\n' >&2
        exit 2
    fi
}

metric() {
    local file="$1"
    local name="$2"
    awk -F': ' -v name="${name}" '$1 == name { print $2; exit }' "${file}"
}

run_client_case() {
    local implementation="$1"
    local round="$2"
    local binary raw_file rc success failed qps p50 p95 p99 valid
    if [[ "${implementation}" == "galay" ]]; then
        binary="${GALAY_BIN}"
    else
        binary="${MYSQLCLIENT_BIN}"
    fi
    raw_file="${RAW_DIR}/${implementation}_select1_run${round}.txt"

    set +e
    "${binary}" --clients "${CLIENTS}" --queries "${QUERIES}" --warmup "${WARMUP}" \
        --timeout-sec "${TIMEOUT_SECONDS}" --mode normal --sql "${SQL}" >"${raw_file}" 2>&1
    rc=$?
    set -e

    success="$(metric "${raw_file}" success)"
    failed="$(metric "${raw_file}" failed)"
    qps="$(metric "${raw_file}" qps)"
    p50="$(metric "${raw_file}" p50_latency_ms)"
    p95="$(metric "${raw_file}" p95_latency_ms)"
    p99="$(metric "${raw_file}" p99_latency_ms)"
    valid=false
    if [[ "${rc}" -eq 0 && "${success:-0}" -gt 0 && "${failed:-1}" -eq 0 ]] && \
       awk -v qps="${qps:-0}" 'BEGIN { exit !(qps > 0) }'; then
        valid=true
    else
        INVALID_CASES=$((INVALID_CASES + 1))
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${implementation}" "${round}" "${CLIENTS}" "${QUERIES}" "${SQL}" \
        "${success:-0}" "${failed:-0}" "${qps:-0}" "${p50:-0}" "${p95:-0}" "${p99:-0}" \
        "${valid}" "${raw_file#${ROOT_DIR}/}" >>"${RESULT_CSV}"
}

run_mysqlslap_case() {
    local round="$1"
    local raw_file rc average_seconds qps valid total_queries
    local password_arg
    raw_file="${RAW_DIR}/mysqlslap_select1_run${round}.txt"
    total_queries=$((CLIENTS * QUERIES))
    password_arg="--password=${GALAY_MYSQL_PASSWORD}"

    set +e
    mysqlslap --no-defaults --host="${GALAY_MYSQL_HOST}" --port="${GALAY_MYSQL_PORT}" \
        --user="${GALAY_MYSQL_USER}" "${password_arg}" --create-schema="${GALAY_MYSQL_DB}" \
        --concurrency="${CLIENTS}" --iterations=1 --number-of-queries="${total_queries}" \
        --query="${SQL}" >"${raw_file}" 2>&1
    rc=$?
    set -e

    average_seconds="$(awk -F': ' '/Average number of seconds to run all queries/ { print $2; exit }' \
        "${raw_file}" | awk '{print $1}')"
    qps="$(awk -v total="${total_queries}" -v seconds="${average_seconds:-0}" \
        'BEGIN { if (seconds > 0) printf "%.3f", total / seconds; else print 0 }')"
    valid=false
    if [[ "${rc}" -eq 0 ]] && awk -v qps="${qps}" 'BEGIN { exit !(qps > 0) }'; then
        valid=true
    else
        INVALID_CASES=$((INVALID_CASES + 1))
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${round}" "${CLIENTS}" "${total_queries}" "${SQL}" "${average_seconds:-0}" \
        "${qps}" "${valid}" "${raw_file#${ROOT_DIR}/}" >>"${MYSQLSLAP_CSV}"
}

require_cmd mysql
require_cmd mysqlslap
require_release_build
mkdir -p "${RAW_DIR}"

mysql_args=(
    --connect-timeout=2
    --protocol=TCP
    "--host=${GALAY_MYSQL_HOST}"
    "--port=${GALAY_MYSQL_PORT}"
    "--user=${GALAY_MYSQL_USER}"
)
if [[ -n "${GALAY_MYSQL_PASSWORD}" ]]; then
    mysql_args+=("--password=${GALAY_MYSQL_PASSWORD}")
fi
if ! mysql "${mysql_args[@]}" --database="${GALAY_MYSQL_DB}" -NBe 'SELECT 1' >/dev/null; then
    printf 'MySQL connection probe failed\n' >&2
    exit 2
fi

printf 'implementation,round,clients,queries_per_client,sql,success,failed,qps,p50_latency_ms,p95_latency_ms,p99_latency_ms,valid,raw_file\n' >"${RESULT_CSV}"
printf 'round,clients,total_queries,sql,average_seconds,qps,valid,raw_file\n' >"${MYSQLSLAP_CSV}"

for round in $(seq 1 "${RUNS}"); do
    run_client_case galay "${round}"
    run_client_case libmysqlclient "${round}"
    run_mysqlslap_case "${round}"
done

printf 'MySQL client comparison CSV: %s\n' "${RESULT_CSV}"
printf 'mysqlslap comparison CSV: %s\n' "${MYSQLSLAP_CSV}"
if [[ "${INVALID_CASES}" -ne 0 ]]; then
    printf 'invalid cases: %s\n' "${INVALID_CASES}" >&2
    exit 1
fi
