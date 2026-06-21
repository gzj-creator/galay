#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULT_DIR="${ROOT_DIR}/benchmark/results/rpc"
REQUESTS="${RPC_BENCH_REQUESTS:-10000}"
PAYLOAD="${RPC_BENCH_PAYLOAD:-128}"

mkdir -p "${RESULT_DIR}"

{
  echo "# RPC open-source comparison"
  date
  uname -a
  echo "requests=${REQUESTS}"
  echo "payload=${PAYLOAD}"
  echo

  if command -v grpc_cpp_plugin >/dev/null 2>&1 && command -v protoc >/dev/null 2>&1; then
    echo "baseline=gRPC C++ toolchain detected"
    echo "status=toolchain_available"
    echo "note=repository does not vendor a gRPC unary echo benchmark; same-config run requires external fixture"
  elif command -v grpcurl >/dev/null 2>&1; then
    echo "baseline=grpcurl detected"
    echo "status=client_available_server_missing"
    echo "note=no local gRPC echo server fixture configured"
  else
    echo "baseline=none"
    echo "status=blocked"
    echo "blocker=no local open-source C++ RPC baseline toolchain detected"
  fi

  echo
  if [[ -x "${ROOT_DIR}/build-rpc-release/benchmark/cpp/rpc/benchmark_rpc_payload_scaling" ]]; then
    echo "galay_payload_scaling_reference:"
    "${ROOT_DIR}/build-rpc-release/benchmark/cpp/rpc/benchmark_rpc_payload_scaling" "${REQUESTS}" "${PAYLOAD}"
  else
    echo "galay_payload_scaling_reference=missing; run scripts/rpc_release_benchmark.sh first"
  fi
} | tee "${RESULT_DIR}/open-source-comparison.txt"
