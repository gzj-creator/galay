#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root}"

python3 scripts/audit_examples_tests_benchmarks_scripts_style.py

cmake -S . -B build/verify-examples-tests-benchmarks-scripts-style \
  -DGALAY_BUILD_UTILS=ON \
  -DGALAY_BUILD_KERNEL=ON \
  -DGALAY_BUILD_HTTP=ON \
  -DGALAY_BUILD_WS=ON \
  -DGALAY_BUILD_HTTP2=ON \
  -DGALAY_BUILD_REDIS=ON \
  -DGALAY_BUILD_RPC=ON \
  -DGALAY_BUILD_EXAMPLES=ON \
  -DGALAY_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=ON

cmake --build build/verify-examples-tests-benchmarks-scripts-style -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
ctest --test-dir build/verify-examples-tests-benchmarks-scripts-style -N
