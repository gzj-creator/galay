#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODE="${1:-}"

DURATION="${DURATION:-30}"
WARMUP="${WARMUP:-5}"
RUNS="${RUNS:-3}"
SERVER_CPUS="${SERVER_CPUS:-0-1}"
CLIENT_CPUS="${CLIENT_CPUS:-2-3}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-19090}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/benchmark-results/transport}"

UDP_CLIENTS="${UDP_CLIENTS:-100}"
UDP_MESSAGES="${UDP_MESSAGES:-100000000}"
UDP_SIZE="${UDP_SIZE:-256}"
WS_CLIENTS="${WS_CLIENTS:-32}"
WS_SIZE="${WS_SIZE:-1024}"
WS_SERVER_IO_THREADS="${WS_SERVER_IO_THREADS:-1}"

SERVER_BINARY="${SERVER_BINARY:-}"
CLIENT_BINARY="${CLIENT_BINARY:-}"
SERVER_PID=""
TMP_DIR=""

usage() {
    echo "usage: $0 udp|ws"
    echo "required env: SERVER_BINARY=/path/to/server CLIENT_BINARY=/path/to/client"
    echo "optional env: DURATION=30 WARMUP=5 RUNS=3 SERVER_CPUS=0-1 CLIENT_CPUS=2-3"
}

cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        rm -rf "$TMP_DIR"
    fi
}
trap cleanup EXIT

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing command: $1" >&2
        exit 127
    fi
}

require_positive_integer() {
    local name="$1"
    local value="$2"
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "$name must be a positive integer: $value" >&2
        exit 2
    fi
}

wait_for_server_log() {
    local log_file="$1"
    local marker="$2"
    local deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
        if grep -Fq "$marker" "$log_file"; then
            return 0
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "server exited before readiness marker: $marker" >&2
            return 1
        fi
        sleep 0.05
    done
    echo "server readiness timed out: $marker" >&2
    return 1
}

summarize_samples() {
    local sample_file="$1"
    local label="$2"
    local sorted_file="$TMP_DIR/${label}.sorted"
    sort -n "$sample_file" >"$sorted_file"
    local count
    count="$(wc -l <"$sorted_file" | tr -d ' ')"
    local middle=$(( (count + 1) / 2 ))
    local median
    median="$(awk -v middle="$middle" 'NR == middle { print; exit }' "$sorted_file")"
    local cv
    cv="$(awk '
        { sum += $1; sum_sq += $1 * $1; count++ }
        END {
            if (count == 0 || sum == 0) {
                print "nan"
                exit
            }
            mean = sum / count
            variance = (sum_sq / count) - (mean * mean)
            if (variance < 0) variance = 0
            printf "%.3f", (sqrt(variance) / mean) * 100
        }
    ' "$sample_file")"
    echo "$label median=$median cv=${cv}% samples=$(tr '\n' ',' <"$sample_file" | sed 's/,$//')"
}

run_udp_client() {
    local duration="$1"
    local output="$2"
    taskset -c "$CLIENT_CPUS" "$CLIENT_BINARY" \
        --host "$HOST" \
        --port "$PORT" \
        --clients "$UDP_CLIENTS" \
        --messages "$UDP_MESSAGES" \
        --size "$UDP_SIZE" \
        --duration "$duration" >"$output" 2>&1
}

run_ws_client() {
    local duration="$1"
    local output="$2"
    taskset -c "$CLIENT_CPUS" "$CLIENT_BINARY" \
        "$WS_CLIENTS" "$duration" "$WS_SIZE" "ws://$HOST:$PORT/ws" >"$output" 2>&1
}

if [[ "$MODE" != "udp" && "$MODE" != "ws" ]]; then
    usage
    exit 2
fi
if [[ -z "$SERVER_BINARY" || -z "$CLIENT_BINARY" ]]; then
    usage
    exit 2
fi
if [[ ! -x "$SERVER_BINARY" || ! -x "$CLIENT_BINARY" ]]; then
    echo "server/client benchmark binary is not executable" >&2
    exit 2
fi

require_command taskset
require_command stdbuf
require_command awk
require_command sort
require_positive_integer DURATION "$DURATION"
require_positive_integer WARMUP "$WARMUP"
require_positive_integer RUNS "$RUNS"
if (( RUNS < 3 || RUNS % 2 == 0 )); then
    echo "RUNS must be an odd integer >= 3 so the median is unambiguous" >&2
    exit 2
fi

mkdir -p "$OUT_DIR"
TMP_DIR="$(mktemp -d)"
STAMP="$(date +%Y%m%d-%H%M%S)"
SERVER_LOG="$OUT_DIR/${MODE}-${STAMP}-server.log"
SAMPLE_FILE="$OUT_DIR/${MODE}-${STAMP}-throughput.samples"
: >"$SAMPLE_FILE"

echo "mode=$MODE duration=${DURATION}s warmup=${WARMUP}s runs=$RUNS server_cpus=$SERVER_CPUS client_cpus=$CLIENT_CPUS"

if [[ "$MODE" == "udp" ]]; then
    taskset -c "$SERVER_CPUS" stdbuf -oL -eL "$SERVER_BINARY" "$PORT" >"$SERVER_LOG" 2>&1 &
    SERVER_PID="$!"
    wait_for_server_log "$SERVER_LOG" "UDP Server workers started"

    run_udp_client "$WARMUP" "$TMP_DIR/warmup.log"
    for run in $(seq 1 "$RUNS"); do
        output="$OUT_DIR/${MODE}-${STAMP}-run${run}.log"
        run_udp_client "$DURATION" "$output"
        throughput="$(awk '/Received: .* pkt\/s/ { print $(NF - 1); exit }' "$output" | tr -d ',')"
        loss="$(awk '/Packet Loss Rate:/ { value = $NF; gsub(/%/, "", value); print value; exit }' "$output")"
        if [[ -z "$throughput" || -z "$loss" ]]; then
            echo "failed to parse UDP result from $output" >&2
            exit 1
        fi
        if ! awk -v loss="$loss" \
            'BEGIN { exit !(loss ~ /^[0-9]+([.][0-9]+)?$/ && loss + 0 == 0) }'; then
            echo "invalid UDP result with packet loss: ${loss}% ($output)" >&2
            exit 1
        fi
        echo "$throughput" >>"$SAMPLE_FILE"
        echo "run=$run throughput=${throughput}pkt/s loss=${loss}%"
    done
    summarize_samples "$SAMPLE_FILE" "udp_pkt_s"
else
    taskset -c "$SERVER_CPUS" stdbuf -oL -eL "$SERVER_BINARY" "$PORT" "$WS_SERVER_IO_THREADS" >"$SERVER_LOG" 2>&1 &
    SERVER_PID="$!"
    wait_for_server_log "$SERVER_LOG" "Server started successfully!"

    run_ws_client "$WARMUP" "$TMP_DIR/warmup.log"
    for run in $(seq 1 "$RUNS"); do
        output="$OUT_DIR/${MODE}-${STAMP}-run${run}.log"
        run_ws_client "$DURATION" "$output"
        throughput="$(awk '/Messages\/sec:/ { print $2; exit }' "$output" | tr -d ',')"
        failed="$(awk '/Failed:/ { print $2; exit }' "$output")"
        if [[ -z "$throughput" || -z "$failed" ]]; then
            echo "failed to parse WebSocket result from $output" >&2
            exit 1
        fi
        if ! awk -v failed="$failed" \
            'BEGIN { exit !(failed ~ /^[0-9]+$/ && failed + 0 == 0) }'; then
            echo "invalid WebSocket result with failed requests: $failed ($output)" >&2
            exit 1
        fi
        echo "$throughput" >>"$SAMPLE_FILE"
        echo "run=$run throughput=${throughput}msg/s failed=$failed"
    done
    summarize_samples "$SAMPLE_FILE" "ws_msg_s"
fi
