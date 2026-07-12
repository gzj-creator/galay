#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODE="${1:---post-echo}"
if [[ "$MODE" == "--post-echo-matrix" || "$MODE" == "--best-post-echo" ]]; then
    MODE="--post-echo-best"
fi
if [[ "$MODE" == "--post-echo-best" && -z "${BUILD_DIR+x}" ]]; then
    BUILD_DIR="$ROOT_DIR/build-release"
else
    BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
fi
DEFAULT_GALAY_SERVER="$BUILD_DIR/benchmark/cpp/http2/benchmark_http2_h2_multiplex_server_throughput"
DEFAULT_GALAY_STATIC_SERVER="$BUILD_DIR/benchmark/cpp/http2/benchmark_http2_h2_static_fast_path"
if [[ ! -x "$DEFAULT_GALAY_SERVER" && -x "$BUILD_DIR/benchmark/http2/benchmark_http2_h2_multiplex_server_throughput" ]]; then
    DEFAULT_GALAY_SERVER="$BUILD_DIR/benchmark/http2/benchmark_http2_h2_multiplex_server_throughput"
fi
if [[ ! -x "$DEFAULT_GALAY_STATIC_SERVER" && -x "$BUILD_DIR/benchmark/http2/benchmark_http2_h2_static_fast_path" ]]; then
    DEFAULT_GALAY_STATIC_SERVER="$BUILD_DIR/benchmark/http2/benchmark_http2_h2_static_fast_path"
fi
GALAY_SERVER="${GALAY_SERVER:-"$DEFAULT_GALAY_SERVER"}"
GALAY_STATIC_SERVER="${GALAY_STATIC_SERVER:-"$DEFAULT_GALAY_STATIC_SERVER"}"
GALAY_PORT="${GALAY_PORT:-9080}"
NGHTTPD_PORT="${NGHTTPD_PORT:-9081}"
HOST="${HOST:-127.0.0.1}"
if [[ "$MODE" == "--static-empty" ]]; then
    MODE="--galay-static-empty"
fi
if [[ "$MODE" == "--all" ]]; then
    "$0" --post-echo
    "$0" --galay-static-empty
    "$0" --galay-static-small
    "$0" --static-files
    exit 0
fi

DURATION="${DURATION:-10}"
WARM_UP="${WARM_UP:-2}"
H2LOAD_THREADS="${H2LOAD_THREADS:-4}"
H2LOAD_CLIENTS="${H2LOAD_CLIENTS:-20}"
H2LOAD_MAX_STREAMS="${H2LOAD_MAX_STREAMS:-100}"
H2LOAD_WINDOW_BITS="${H2LOAD_WINDOW_BITS:-}"
H2LOAD_REQUESTS="${H2LOAD_REQUESTS:-}"
SERVER_IO_THREADS="${SERVER_IO_THREADS:-4}"
SERVER_MAX_STREAMS="${SERVER_MAX_STREAMS:-1000}"
SERVER_IO_THREADS_LIST="${SERVER_IO_THREADS_LIST:-1 2 4 8}"
SERVER_MAX_STREAMS_LIST="${SERVER_MAX_STREAMS_LIST:-$SERVER_MAX_STREAMS}"
H2LOAD_THREADS_LIST="${H2LOAD_THREADS_LIST:-$H2LOAD_THREADS}"
H2LOAD_CLIENTS_LIST="${H2LOAD_CLIENTS_LIST:-20 40 80}"
H2LOAD_MAX_STREAMS_LIST="${H2LOAD_MAX_STREAMS_LIST:-100 250}"
NGHTTPD_EXTRA_ARGS="${NGHTTPD_EXTRA_ARGS:-}"
H2LOAD_SUPPORTS_HISTOGRAM=0

TMP_DIR="$(mktemp -d)"
PAYLOAD_FILE="$TMP_DIR/payload"
GALAY_PID=""
NGHTTPD_PID=""
LAST_REQ_S="0"
LAST_P95="n/a"
LAST_P99="n/a"
LAST_FAILED="0"
LAST_ERRORED="0"
LAST_TIMEOUT="0"
LAST_AVG_CPU="0.0"
LAST_MAX_CPU="0.0"
LAST_MAX_RSS="0.0"

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

cmake_build_type() {
    local cache="$1/CMakeCache.txt"
    if [[ ! -f "$cache" ]]; then
        echo "unknown"
        return 0
    fi
    awk -F= '
        /^CMAKE_BUILD_TYPE:/ {
            print $2
            found = 1
        }
        END {
            if (!found) print "unknown"
        }
    ' "$cache"
}

require_release_build_for_best() {
    local build_type="$1"
    if [[ "${ALLOW_NON_RELEASE:-0}" == "1" ]]; then
        return 0
    fi
    if [[ "$build_type" == "Release" ]]; then
        return 0
    fi

    echo "best-of mode requires a Release galay build by default." >&2
    echo "current BUILD_DIR: $BUILD_DIR" >&2
    echo "current CMAKE_BUILD_TYPE: $build_type" >&2
    echo "configure one first:" >&2
    echo "  cmake -S \"$ROOT_DIR\" -B \"$ROOT_DIR/build-release\" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DGALAY_BUILD_BENCHMARKS=ON" >&2
    echo "  cmake --build \"$ROOT_DIR/build-release\" --target benchmark_http2_h2_multiplex_server_throughput" >&2
    echo "or set ALLOW_NON_RELEASE=1 if this is only a smoke run." >&2
    exit 1
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
    if [[ "$H2LOAD_SUPPORTS_HISTOGRAM" == "1" ]]; then
        read -r p95 p99 < <(awk '/^request[[:space:]]*:/ { print $6, $7 }' "$output" | tail -n1)
    else
        p95="unavailable"
        p99="unavailable"
    fi

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

    local h2load_args=()
    h2load_args=(-t"$H2LOAD_THREADS" -c"$H2LOAD_CLIENTS" -m"$H2LOAD_MAX_STREAMS")
    if [[ "$H2LOAD_SUPPORTS_HISTOGRAM" == "1" ]]; then
        h2load_args+=(--histogram)
    fi
    if [[ -n "$H2LOAD_WINDOW_BITS" ]]; then
        h2load_args+=(-w"$H2LOAD_WINDOW_BITS")
    fi

    set +e
    if [[ -n "$H2LOAD_REQUESTS" ]]; then
        if [[ "$method" == "POST" ]]; then
            h2load -n"$H2LOAD_REQUESTS" \
                "${h2load_args[@]}" \
                -d "$PAYLOAD_FILE" "http://$HOST:$port$path" >"$output" 2>&1
        else
            h2load -n"$H2LOAD_REQUESTS" \
                "${h2load_args[@]}" \
                "http://$HOST:$port$path" >"$output" 2>&1
        fi
    else
        if [[ "$method" == "POST" ]]; then
            h2load -D"$DURATION" --warm-up-time="$WARM_UP" \
                "${h2load_args[@]}" \
                -d "$PAYLOAD_FILE" "http://$HOST:$port$path" >"$output" 2>&1
        else
            h2load -D"$DURATION" --warm-up-time="$WARM_UP" \
                "${h2load_args[@]}" \
                "http://$HOST:$port$path" >"$output" 2>&1
        fi
    fi
    rc="$?"
    set -e

    kill "$sample_pid" 2>/dev/null || true
    wait "$sample_pid" 2>/dev/null || true

    read -r req_s p95 p99 failed errored timeout < <(parse_h2load "$output")
    read -r avg_cpu max_cpu max_rss < <(resource_summary "$samples")

    LAST_REQ_S="$req_s"
    LAST_P95="$p95"
    LAST_P99="$p99"
    LAST_FAILED="$failed"
    LAST_ERRORED="$errored"
    LAST_TIMEOUT="$timeout"
    LAST_AVG_CPU="$avg_cpu"
    LAST_MAX_CPU="$max_cpu"
    LAST_MAX_RSS="$max_rss"

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

run_post_echo_server_case() {
    local impl="$1"
    local server_threads="$2"
    local server_max_streams="$3"
    local label="$4"

    if [[ "$impl" == "galay" ]]; then
        "$GALAY_SERVER" "$GALAY_PORT" "$server_threads" "$server_max_streams" 0 \
            >"$TMP_DIR/${label}.server.log" 2>&1 &
        GALAY_PID="$!"
        if ! wait_for_port "$GALAY_PORT"; then
            kill "$GALAY_PID" 2>/dev/null || true
            wait "$GALAY_PID" 2>/dev/null || true
            GALAY_PID=""
            return 1
        fi
        run_h2load_case "$label" "$GALAY_PORT" "$GALAY_PID" "/echo" "POST"
        local rc="$?"
        kill "$GALAY_PID" 2>/dev/null || true
        wait "$GALAY_PID" 2>/dev/null || true
        GALAY_PID=""
        return "$rc"
    fi

    if [[ -n "$NGHTTPD_EXTRA_ARGS" ]]; then
        # shellcheck disable=SC2086
        nghttpd --no-tls -a "$HOST" -n "$server_threads" -m "$server_max_streams" \
            $NGHTTPD_EXTRA_ARGS --echo-upload "$NGHTTPD_PORT" \
            >"$TMP_DIR/${label}.server.log" 2>&1 &
    else
        nghttpd --no-tls -a "$HOST" -n "$server_threads" -m "$server_max_streams" \
            --echo-upload "$NGHTTPD_PORT" >"$TMP_DIR/${label}.server.log" 2>&1 &
    fi
    NGHTTPD_PID="$!"
    if ! wait_for_port "$NGHTTPD_PORT"; then
        kill "$NGHTTPD_PID" 2>/dev/null || true
        wait "$NGHTTPD_PID" 2>/dev/null || true
        NGHTTPD_PID=""
        return 1
    fi
    run_h2load_case "$label" "$NGHTTPD_PORT" "$NGHTTPD_PID" "/echo" "POST"
    local rc="$?"
    kill "$NGHTTPD_PID" 2>/dev/null || true
    wait "$NGHTTPD_PID" 2>/dev/null || true
    NGHTTPD_PID=""
    return "$rc"
}

run_post_echo_best_for_impl() {
    local impl="$1"
    local summary_file="$2"
    local best_req_s="0"
    local best_label=""
    local best_p95="n/a"
    local best_p99="n/a"
    local best_avg_cpu="0.0"
    local best_max_cpu="0.0"
    local best_max_rss="0.0"
    local best_server_threads="0"
    local best_server_max_streams="0"
    local best_h2load_threads="0"
    local best_h2load_clients="0"
    local best_h2load_max_streams="0"

    for server_threads in $SERVER_IO_THREADS_LIST; do
        for server_max_streams in $SERVER_MAX_STREAMS_LIST; do
            for h2load_threads in $H2LOAD_THREADS_LIST; do
                for h2load_clients in $H2LOAD_CLIENTS_LIST; do
                    for h2load_max_streams in $H2LOAD_MAX_STREAMS_LIST; do
                        H2LOAD_THREADS="$h2load_threads"
                        H2LOAD_CLIENTS="$h2load_clients"
                        H2LOAD_MAX_STREAMS="$h2load_max_streams"
                        local label="${impl}-s${server_threads}-sm${server_max_streams}-t${h2load_threads}-c${h2load_clients}-m${h2load_max_streams}"

                        if ! run_post_echo_server_case "$impl" "$server_threads" "$server_max_streams" "$label"; then
                            continue
                        fi
                        if [[ "$LAST_FAILED" != "0" || "$LAST_ERRORED" != "0" || "$LAST_TIMEOUT" != "0" ]]; then
                            continue
                        fi

                        if awk -v current="$LAST_REQ_S" -v best="$best_req_s" 'BEGIN { exit !(current > best) }'; then
                            best_req_s="$LAST_REQ_S"
                            best_label="$label"
                            best_p95="$LAST_P95"
                            best_p99="$LAST_P99"
                            best_avg_cpu="$LAST_AVG_CPU"
                            best_max_cpu="$LAST_MAX_CPU"
                            best_max_rss="$LAST_MAX_RSS"
                            best_server_threads="$server_threads"
                            best_server_max_streams="$server_max_streams"
                            best_h2load_threads="$h2load_threads"
                            best_h2load_clients="$h2load_clients"
                            best_h2load_max_streams="$h2load_max_streams"
                        fi
                    done
                done
            done
        done
    done

    if [[ -z "$best_label" ]]; then
        echo "no successful $impl post-echo matrix case" >&2
        return 1
    fi

    printf "%s %s %s %s %s %s %s %s %s %s %s %s\n" \
        "$impl" "$best_req_s" "$best_p95" "$best_p99" "$best_avg_cpu" "$best_max_cpu" "$best_max_rss" \
        "$best_server_threads" "$best_server_max_streams" "$best_h2load_threads" "$best_h2load_clients" \
        "$best_h2load_max_streams" >>"$summary_file"

    echo
    echo "best $impl: req/s=$best_req_s p95=$best_p95 p99=$best_p99 avg_cpu=$best_avg_cpu max_cpu=$best_max_cpu rss_mib=$best_max_rss"
    echo "best $impl config: server_threads=$best_server_threads server_max_streams=$best_server_max_streams h2load_threads=$best_h2load_threads clients=$best_h2load_clients streams=$best_h2load_max_streams"
}

run_post_echo_best_matrix() {
    local summary_file="$TMP_DIR/post-echo-best-summary.txt"
    : >"$summary_file"

    run_post_echo_best_for_impl "galay" "$summary_file"
    run_post_echo_best_for_impl "nghttpd" "$summary_file"

    echo
    printf "%-10s %-15s %12s %10s %10s %10s %10s %12s %14s %18s %16s %8s %8s\n" \
        "impl" "role" "best_req/s" "p95" "p99" "avg_cpu%" "max_cpu%" "max_rss_mib" \
        "server_threads" "server_max_streams" "h2load_threads" "clients" "streams"
    awk '{
        role = ($1 == "nghttpd") ? "reference_only" : "candidate"
        printf "%-10s %-15s %12s %10s %10s %10s %10s %12s %14s %18s %16s %8s %8s\n",
               $1, role, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12
    }' "$summary_file"
    echo
    echo "ranking: disabled; nghttpd --echo-upload is a demo interoperability reference, not a scalable echo competitor"
}

require_cmd h2load
require_cmd nghttpd
require_cmd ps

H2LOAD_HELP="$(h2load --help 2>&1)"
if grep -q -- '--histogram' <<<"$H2LOAD_HELP"; then
    H2LOAD_SUPPORTS_HISTOGRAM=1
fi
unset H2LOAD_HELP

if [[ "$MODE" != "--post-echo" &&
      "$MODE" != "--post-echo-best" &&
      "$MODE" != "--galay-static-empty" &&
      "$MODE" != "--galay-static-small" &&
      "$MODE" != "--static-files" ]]; then
    echo "usage: $0 [--post-echo|--post-echo-best|--post-echo-matrix|--galay-static-empty|--static-empty|--galay-static-small|--static-files|--all]" >&2
    exit 2
fi

BUILD_TYPE="$(cmake_build_type "$BUILD_DIR")"
if [[ "$MODE" == "--post-echo-best" ]]; then
    require_release_build_for_best "$BUILD_TYPE"
fi

if [[ ("$MODE" == "--post-echo" || "$MODE" == "--post-echo-best") && ! -x "$GALAY_SERVER" ]]; then
    echo "missing executable: $GALAY_SERVER" >&2
    echo "build it first: cmake --build \"$BUILD_DIR\" --target benchmark_http2_h2_multiplex_server_throughput" >&2
    exit 1
fi
if [[ "$MODE" != "--post-echo" && "$MODE" != "--post-echo-best" && ! -x "$GALAY_STATIC_SERVER" ]]; then
    echo "missing executable: $GALAY_STATIC_SERVER" >&2
    echo "build it first: cmake --build \"$BUILD_DIR\" --target benchmark_http2_h2_static_fast_path" >&2
    exit 1
fi

printf "hello-h2c-mux" >"$PAYLOAD_FILE"

echo "HTTP/2 h2load external probe"
echo "h2load:  $(h2load --version)"
echo "nghttpd: $(nghttpd --version)"
if [[ "$H2LOAD_SUPPORTS_HISTOGRAM" == "1" ]]; then
    echo "latency_percentiles=p95,p99"
else
    echo "latency_percentiles=unavailable"
fi
echo "build:   $BUILD_DIR (CMAKE_BUILD_TYPE=$BUILD_TYPE)"
echo "payload: $(wc -c <"$PAYLOAD_FILE" | tr -d ' ') bytes"
if [[ "$MODE" == "--post-echo" || "$MODE" == "--post-echo-best" ]]; then
    echo "mode:    POST echo"
    echo "fairness: nghttpd --echo-upload is a reference-only interoperability probe; do not use for competitor ranking"
    if [[ "$MODE" == "--post-echo-best" ]]; then
        echo "matrix:  server_threads=[$SERVER_IO_THREADS_LIST], server_max_streams=[$SERVER_MAX_STREAMS_LIST]"
        echo "matrix:  h2load_threads=[$H2LOAD_THREADS_LIST], clients=[$H2LOAD_CLIENTS_LIST], streams=[$H2LOAD_MAX_STREAMS_LIST]"
        if [[ -n "$NGHTTPD_EXTRA_ARGS" ]]; then
            echo "nghttpd extra args: $NGHTTPD_EXTRA_ARGS"
        fi
    else
        echo "h2load:  -D$DURATION --warm-up-time=$WARM_UP -t$H2LOAD_THREADS -c$H2LOAD_CLIENTS -m$H2LOAD_MAX_STREAMS -d payload"
    fi
else
    if [[ "$MODE" == "--static-files" ]]; then
        echo "mode:    GET static files 0B/1KB/16KB/128KB/1MB"
        if [[ -z "$H2LOAD_WINDOW_BITS" ]]; then
            H2LOAD_WINDOW_BITS=21
        fi
        if [[ -z "$H2LOAD_REQUESTS" ]]; then
            H2LOAD_REQUESTS=2000
        fi
    elif [[ "$MODE" == "--galay-static-small" ]]; then
        echo "mode:    GET static 1KB"
    else
        echo "mode:    GET static empty"
    fi
    if [[ -n "$H2LOAD_REQUESTS" && -n "$H2LOAD_WINDOW_BITS" ]]; then
        echo "h2load:  -n$H2LOAD_REQUESTS -t$H2LOAD_THREADS -c$H2LOAD_CLIENTS -m$H2LOAD_MAX_STREAMS -w$H2LOAD_WINDOW_BITS"
    elif [[ -n "$H2LOAD_REQUESTS" ]]; then
        echo "h2load:  -n$H2LOAD_REQUESTS -t$H2LOAD_THREADS -c$H2LOAD_CLIENTS -m$H2LOAD_MAX_STREAMS"
    elif [[ -n "$H2LOAD_WINDOW_BITS" ]]; then
        echo "h2load:  -D$DURATION --warm-up-time=$WARM_UP -t$H2LOAD_THREADS -c$H2LOAD_CLIENTS -m$H2LOAD_MAX_STREAMS -w$H2LOAD_WINDOW_BITS"
    else
        echo "h2load:  -D$DURATION --warm-up-time=$WARM_UP -t$H2LOAD_THREADS -c$H2LOAD_CLIENTS -m$H2LOAD_MAX_STREAMS"
    fi
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
    run_h2load_case "nghttpd-echo-upload-ref" "$NGHTTPD_PORT" "$NGHTTPD_PID" "/echo" "POST"
    kill "$NGHTTPD_PID" 2>/dev/null || true
    wait "$NGHTTPD_PID" 2>/dev/null || true
    NGHTTPD_PID=""
elif [[ "$MODE" == "--post-echo-best" ]]; then
    run_post_echo_best_matrix
else
    "$GALAY_STATIC_SERVER" "$GALAY_PORT" "$SERVER_IO_THREADS" "$SERVER_MAX_STREAMS" 0 \
        >"$TMP_DIR/galay-static.server.log" 2>&1 &
    GALAY_PID="$!"
    wait_for_port "$GALAY_PORT"
    if [[ "$MODE" == "--static-files" ]]; then
        run_h2load_case "galay-file-0b" "$GALAY_PORT" "$GALAY_PID" "/files/0b.bin" "GET"
        run_h2load_case "galay-file-1kb" "$GALAY_PORT" "$GALAY_PID" "/files/1kb.bin" "GET"
        run_h2load_case "galay-file-16kb" "$GALAY_PORT" "$GALAY_PID" "/files/16kb.bin" "GET"
        run_h2load_case "galay-file-128kb" "$GALAY_PORT" "$GALAY_PID" "/files/128kb.bin" "GET"
        run_h2load_case "galay-file-1mb" "$GALAY_PORT" "$GALAY_PID" "/files/1mb.bin" "GET"
    elif [[ "$MODE" == "--galay-static-small" ]]; then
        run_h2load_case "galay-static-1kb" "$GALAY_PORT" "$GALAY_PID" "/small" "GET"
    else
        run_h2load_case "galay-static-empty" "$GALAY_PORT" "$GALAY_PID" "/echo" "GET"
    fi
    kill "$GALAY_PID" 2>/dev/null || true
    wait "$GALAY_PID" 2>/dev/null || true
    GALAY_PID=""
fi
