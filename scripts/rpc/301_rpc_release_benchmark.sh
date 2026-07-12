#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${RPC_BUILD_DIR:-${ROOT_DIR}/build-rpc-release}"
RESULT_DIR="${RPC_RESULT_DIR:-${ROOT_DIR}/benchmark/results/rpc}"
REQUESTS="${RPC_BENCH_REQUESTS:-10000}"
PAYLOAD="${RPC_BENCH_PAYLOAD:-128}"

mkdir -p "${RESULT_DIR}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGALAY_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=ON

cmake --build "${BUILD_DIR}" --target \
  benchmark_rpc_unary_loopback_latency \
  benchmark_rpc_stream_pressure \
  benchmark_rpc_unary_concurrent_pressure \
  benchmark_rpc_pool_pressure \
  benchmark_rpc_managed_client_pressure \
  benchmark_rpc_payload_scaling \
  --parallel

{
  echo "# galay-rpc release benchmark"
  date
  uname -a
  echo "requests=${REQUESTS}"
  echo "payload=${PAYLOAD}"
  echo "build_dir=${BUILD_DIR}"
  echo "wire=galay-rpc-custom-frame"
  echo
  echo "scenario=network_unary_loopback"
  "${BUILD_DIR}/benchmark/cpp/rpc/benchmark_rpc_unary_loopback_latency" 1000
  echo
  echo "scenario=in_process_stream_frame_codec"
  "${BUILD_DIR}/benchmark/cpp/rpc/benchmark_rpc_stream_pressure" "${REQUESTS}" "${PAYLOAD}"
  echo
  echo "scenario=in_process_admission_control"
  "${BUILD_DIR}/benchmark/cpp/rpc/benchmark_rpc_unary_concurrent_pressure" "${REQUESTS}"
  echo
  echo "scenario=in_process_pool_state"
  "${BUILD_DIR}/benchmark/cpp/rpc/benchmark_rpc_pool_pressure" "${REQUESTS}"
  echo
  echo "scenario=in_process_managed_client_state"
  "${BUILD_DIR}/benchmark/cpp/rpc/benchmark_rpc_managed_client_pressure" "${REQUESTS}"
  echo
  echo "scenario=in_process_wire_codec"
  "${BUILD_DIR}/benchmark/cpp/rpc/benchmark_rpc_payload_scaling" "${REQUESTS}" "${PAYLOAD}"
} | tee "${RESULT_DIR}/galay-rpc-release.txt"
