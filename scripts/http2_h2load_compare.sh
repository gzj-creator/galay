#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
GALAY_SERVER="${GALAY_SERVER:-"$BUILD_DIR/benchmark/http2/benchmark_http2_h2_multiplex_server_throughput"}"
GALAY_STATIC_SERVER="${GALAY_STATIC_SERVER:-"$BUILD_DIR/benchmark/http2/benchmark_http2_h2_static_fast_path"}"
GALAY_PORT="${GALAY_PORT:-9080}"
NGHTTPD_PORT="${NGHTTPD_PORT:-9081}"
HOST="${HOST:-127.0.0.1}"
MODE="${1:---post-echo}"

DURATION="${DURATION:-10}"
WARM_UP="${WARM_UP:-2}"
H2LOAD_THREADS="${H2LOAD_THREADS:-4}"
H2LOAD_CLIENTS="${H2LOAD_CLIENTS:-20}"
H2LOAD_MAX_STREAMS="${H2LOAD_MAX_STREAMS:-100}"
SERVER_IO_THREADS="${SERVER_IO_THREADS:-4}"
SERVER_MAX_STREAMS="${SERVER_MAX_STREAMS:-1000}"

TMP_DIR="$(mktemp -d)"
PAYLOAD_FILE="$TMP_DIR/payload"
GALAY_PID=""
NGHTTPD_PID=""

cleanup() {
    if [[ -n "$GALAY_PID" ]]; then
        kill "$GALAY_PID" 2>/dev/null || true
        wait "$GALAY_PID" 2>/dev/null || true
    fi
    if [[ -n "$NGHTTPD_PID" ]]; then
        kill "$NGHTTPD_PID" 2>/dev/null || true
        wait "$NGHTTPD_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

require_cmd() {
    local name="$1"
    if ! command -v "$name" >/dev/null 2>&1; then
        echo "missing command: $name" >&2
        exit 127
    fi
}

wait_for_port() {
    local port="$1"
    local deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
        if (echo >/dev/tcp/"$HOST"/"$port") >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    echo "server did not listen on $HOST:$port" >&2
    return 1
}

sample_resources() {
    local pid="$1"
    local output="$2"
    while kill -0 "$pid" >/dev/null 2>&1; do
        ps -o %cpu=,rss= -p "$pid" >>"$output" 2>/dev/null || true
        sleep 0.2
    done
}

resource_summary() {
    local samples="$1"
    awk '
        NF >= 2 {
            cpu += $1
            if ($1 > max_cpu) max_cpu = $1
            if ($2 > max_rss) max_rss = $2
            n++
        }
        END {
            if (n == 0) {
                printf "0.0 0.0 0.0\n"
            } else {
                printf "%.1f %.1f %.1f\n", cpu / n, max_cpu, max_rss / 1024.0
            }
        }
    ' "$samples"
}

parse_h2load() {
    local output="$1"
    local req_s failed errored timeout p95 p99
    req_s="$(awk '/finished in/ { for (i = 1; i <= NF; i++) if ($i == "req/s,") print $(i - 1) }' "$output" | tail -n1 | tr -d ',')"
    failed="$(awk '/^requests:/ { for (i = 1; i <= NF; i++) if ($i == "failed,") print $(i - 1) }' "$output" | tail -n1)"
    errored="$(awk '/^requests:/ { for (i = 1; i <= NF; i++) if ($i == "errored,") print $(i - 1) }' "$output" | tail -n1)"
    timeout="$(awk '/^requests:/ { for (i = 1; i <= NF; i++) if ($i == "timeout") print $(i - 1) }' "$output" | tail -n1)"
    read -r p95 p99 < <(awk '/^request[[:space:]]*:/ { print $6, $7 }' "$output" | tail -n1)

    printf "%s %s %s %s %s %s\n" \
        "${req_s:-0}" "${p95:-n/a}" "${p99:-n/a}" \
        "${failed:-0}" "${errored:-0}" "${timeout:-0}"
}

run_h2load_case() {
    local label="$1"
    local port="$2"
    local server_pid="$3"
    local path="$4"
    local method="$5"
    local output="$TMP_DIR/${label}.h2load.txt"
    local samples="$TMP_DIR/${label}.ps.txt"
    local sample_pid rc req_s p95 p99 failed errored timeout avg_cpu max_cpu max_rss

    : >"$samples"
    sample_resources "$server_pid" "$samples" &
    sample_pid="$!"

    set +e
    if [[ "$method" == "POST" ]]; then
        h2load -D"$DURATION" --warm-up-time="$WARM_UP" --histogram \
            -t"$H2LOAD_THREADS" -c"$H2LOAD_CLIENTS" -m"$H2LOAD_MAX_STREAMS" \
            -d "$PAYLOAD_FILE" "http://$HOST:$port$path" >"$output" 2>&1
    else
        h2load -D"$DURATION" --warm-up-time="$WARM_UP" --histogram \
            -t"$H2LOAD_THREADS" -c"$H2LOAD_CLIENTS" -m"$H2LOAD_MAX_STREAMS" \
            "http://$HOST:$port$path" >"$output" 2>&1
    fi
    rc="$?"
    set -e

    kill "$sample_pid" 2>/dev/null || true
    wait "$sample_pid" 2>/dev/null || true

    read -r req_s p95 p99 failed errored timeout < <(parse_h2load "$output")
    read -r avg_cpu max_cpu max_rss < <(resource_summary "$samples")

    printf "%-22s %12s %10s %10s %8s %8s %8s %10s %10s %12s %6s\n" \
        "$label" "$req_s" "$p95" "$p99" "$failed" "$errored" "$timeout" \
        "$avg_cpu" "$max_cpu" "$max_rss" "$rc"

    if [[ "$rc" -ne 0 ]]; then
        echo
        echo "h2load failed for $label; raw output:"
        sed 's/^/  /' "$output"
        return "$rc"
    fi
}

require_cmd h2load
require_cmd nghttpd
require_cmd ps

if [[ "$MODE" != "--post-echo" &&
      "$MODE" != "--galay-static-empty" &&
      "$MODE" != "--galay-static-small" ]]; then
    echo "usage: $0 [--post-echo|--galay-static-empty|--galay-static-small]" >&2
    exit 2
fi

if [[ "$MODE" == "--post-echo" && ! -x "$GALAY_SERVER" ]]; then
    echo "missing executable: $GALAY_SERVER" >&2
    echo "build it first: cmake --build build --target benchmark_http2_h2_multiplex_server_throughput" >&2
    exit 1
fi
if [[ "$MODE" != "--post-echo" && ! -x "$GALAY_STATIC_SERVER" ]]; then
    echo "missing executable: $GALAY_STATIC_SERVER" >&2
    echo "build it first: cmake --build build --target benchmark_http2_h2_static_fast_path" >&2
    exit 1
fi

printf "hello-h2c-mux" >"$PAYLOAD_FILE"

echo "HTTP/2 h2load external comparison"
echo "h2load:  $(h2load --version)"
echo "nghttpd: $(nghttpd --version)"
echo "payload: $(wc -c <"$PAYLOAD_FILE" | tr -d ' ') bytes"
if [[ "$MODE" == "--post-echo" ]]; then
    echo "mode:    POST echo"
    echo "h2load:  -D$DURATION --warm-up-time=$WARM_UP -t$H2LOAD_THREADS -c$H2LOAD_CLIENTS -m$H2LOAD_MAX_STREAMS -d payload"
else
    if [[ "$MODE" == "--galay-static-small" ]]; then
        echo "mode:    GET static 1KB"
    else
        echo "mode:    GET static empty"
    fi
    echo "h2load:  -D$DURATION --warm-up-time=$WARM_UP -t$H2LOAD_THREADS -c$H2LOAD_CLIENTS -m$H2LOAD_MAX_STREAMS"
fi
echo "server:  $SERVER_IO_THREADS IO workers, max_streams=$SERVER_MAX_STREAMS"
echo
printf "%-22s %12s %10s %10s %8s %8s %8s %10s %10s %12s %6s\n" \
    "scenario" "req/s" "p95" "p99" "failed" "errored" "timeout" \
    "avg_cpu%" "max_cpu%" "max_rss_mib" "rc"

if [[ "$MODE" == "--post-echo" ]]; then
    "$GALAY_SERVER" "$GALAY_PORT" "$SERVER_IO_THREADS" "$SERVER_MAX_STREAMS" 0 \
        >"$TMP_DIR/galay.server.log" 2>&1 &
    GALAY_PID="$!"
    wait_for_port "$GALAY_PORT"
    run_h2load_case "galay-post-echo" "$GALAY_PORT" "$GALAY_PID" "/echo" "POST"
    kill "$GALAY_PID" 2>/dev/null || true
    wait "$GALAY_PID" 2>/dev/null || true
    GALAY_PID=""

    nghttpd --no-tls -a "$HOST" -n "$SERVER_IO_THREADS" -m "$SERVER_MAX_STREAMS" \
        --echo-upload "$NGHTTPD_PORT" >"$TMP_DIR/nghttpd.server.log" 2>&1 &
    NGHTTPD_PID="$!"
    wait_for_port "$NGHTTPD_PORT"
    run_h2load_case "nghttpd-echo-upload" "$NGHTTPD_PORT" "$NGHTTPD_PID" "/echo" "POST"
    kill "$NGHTTPD_PID" 2>/dev/null || true
    wait "$NGHTTPD_PID" 2>/dev/null || true
    NGHTTPD_PID=""
else
    "$GALAY_STATIC_SERVER" "$GALAY_PORT" "$SERVER_IO_THREADS" "$SERVER_MAX_STREAMS" 0 \
        >"$TMP_DIR/galay-static.server.log" 2>&1 &
    GALAY_PID="$!"
    wait_for_port "$GALAY_PORT"
    if [[ "$MODE" == "--galay-static-small" ]]; then
        run_h2load_case "galay-static-1kb" "$GALAY_PORT" "$GALAY_PID" "/small" "GET"
    else
        run_h2load_case "galay-static-empty" "$GALAY_PORT" "$GALAY_PID" "/echo" "GET"
    fi
    kill "$GALAY_PID" 2>/dev/null || true
    wait "$GALAY_PID" 2>/dev/null || true
    GALAY_PID=""
fi
