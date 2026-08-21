#!/usr/bin/env bash
set -euo pipefail

# gRPC/grpcurl use a different wire protocol and are not formal competitors.
# This compatibility entry point intentionally emits a non-ranking result.
printf '%s\n' \
  'status=not_applicable' \
  'competitor=boost.asio coroutine' \
  'scenario=rpc protocol' \
  'reason=No same-workload Boost.Asio coroutine RPC protocol harness is registered; gRPC and other external tools are historical/internal-only.'
