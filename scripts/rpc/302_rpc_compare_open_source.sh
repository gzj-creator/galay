#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${RPC_BUILD_DIR:-${ROOT_DIR}/build-rpc-release}"
RESULT_DIR="${RPC_RESULT_DIR:-${ROOT_DIR}/benchmark/results/rpc}"
REQUESTS="${RPC_BENCH_REQUESTS:-10000}"
PAYLOAD="${RPC_BENCH_PAYLOAD:-128}"
SAME_WIRE_REFERENCE="${RPC_SAME_WIRE_REFERENCE:-}"

mkdir -p "${RESULT_DIR}"

{
  echo "# RPC open-source comparison"
  date
  uname -a
  echo "requests=${REQUESTS}"
  echo "payload=${PAYLOAD}"
  echo "galay_wire=custom-frame"
  echo "ranking_rule=same-wire-only"
  echo

  if [[ -n "${SAME_WIRE_REFERENCE}" && -x "${SAME_WIRE_REFERENCE}" ]]; then
    echo "same_wire_status=available"
    echo "same_wire_reference=${SAME_WIRE_REFERENCE}"
    "${SAME_WIRE_REFERENCE}" "${REQUESTS}" "${PAYLOAD}"
  else
    echo "same_wire_status=blocked"
    echo "same_wire_blocker=RPC_SAME_WIRE_REFERENCE is not an executable fixture"
  fi

  echo
  echo "grpc_comparison_class=sidecar_not_ranked"
  echo "grpc_ranking_allowed=false"
  echo "grpc_reason=gRPC uses HTTP/2 and protobuf rather than the galay custom wire"

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
  if [[ -x "${BUILD_DIR}/benchmark/cpp/rpc/benchmark_rpc_payload_scaling" ]]; then
    echo "galay_payload_scaling_reference:"
    "${BUILD_DIR}/benchmark/cpp/rpc/benchmark_rpc_payload_scaling" "${REQUESTS}" "${PAYLOAD}"
  else
    echo "galay_payload_scaling_reference=missing; run scripts/rpc/301_rpc_release_benchmark.sh first"
  fi
} | tee "${RESULT_DIR}/open-source-comparison.txt"
