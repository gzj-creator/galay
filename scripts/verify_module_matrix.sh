#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"

minimal_build="${root}/build/verify-minimal"
protocol_build="${root}/build/verify-protocols-no-ssl"
ssl_build="${root}/build/verify-ssl"

rm -rf "${minimal_build}" "${protocol_build}" "${ssl_build}"

cmake -S "${root}" -B "${minimal_build}" \
  -DGALAY_BUILD_UTILS=ON \
  -DGALAY_BUILD_KERNEL=ON \
  -DGALAY_BUILD_SSL=OFF \
  -DGALAY_BUILD_HTTP=OFF \
  -DGALAY_BUILD_WS=OFF \
  -DGALAY_BUILD_HTTP2=OFF \
  -DGALAY_BUILD_REDIS=OFF \
  -DGALAY_BUILD_ETCD=OFF \
  -DGALAY_BUILD_MONGO=OFF \
  -DGALAY_BUILD_MYSQL=OFF \
  -DGALAY_BUILD_RPC=OFF \
  -DGALAY_BUILD_MCP=OFF \
  -DGALAY_BUILD_TRACING=OFF \
  -DBUILD_TESTING=OFF
cmake --build "${minimal_build}" --target utils kernel -j "${jobs}"
if grep -E 'OpenSSL|OPENSSL' "${minimal_build}/CMakeCache.txt" >/dev/null; then
  echo "minimal build unexpectedly probed OpenSSL" >&2
  exit 1
fi

cmake -S "${root}" -B "${protocol_build}" \
  -DGALAY_BUILD_UTILS=ON \
  -DGALAY_BUILD_KERNEL=ON \
  -DGALAY_BUILD_HTTP=ON \
  -DGALAY_BUILD_WS=ON \
  -DGALAY_BUILD_HTTP2=ON \
  -DGALAY_BUILD_SSL=OFF \
  -DGALAY_BUILD_REDIS=ON \
  -DGALAY_BUILD_ETCD=OFF \
  -DGALAY_BUILD_MONGO=OFF \
  -DGALAY_BUILD_MYSQL=OFF \
  -DGALAY_BUILD_RPC=OFF \
  -DGALAY_BUILD_MCP=OFF \
  -DGALAY_BUILD_TRACING=OFF \
  -DBUILD_TESTING=OFF
cmake --build "${protocol_build}" --target http ws http2 redis -j "${jobs}"
if grep -E 'OpenSSL|OPENSSL' "${protocol_build}/CMakeCache.txt" >/dev/null; then
  echo "protocol modules without SSL unexpectedly probed OpenSSL" >&2
  exit 1
fi

cmake -S "${root}" -B "${ssl_build}" \
  -DGALAY_BUILD_UTILS=ON \
  -DGALAY_BUILD_KERNEL=ON \
  -DGALAY_BUILD_SSL=ON \
  -DGALAY_BUILD_HTTP=ON \
  -DGALAY_BUILD_WS=ON \
  -DGALAY_BUILD_HTTP2=ON \
  -DGALAY_BUILD_REDIS=ON \
  -DGALAY_BUILD_ETCD=OFF \
  -DGALAY_BUILD_MONGO=OFF \
  -DGALAY_BUILD_MYSQL=OFF \
  -DGALAY_BUILD_RPC=OFF \
  -DGALAY_BUILD_MCP=OFF \
  -DGALAY_BUILD_TRACING=OFF \
  -DBUILD_TESTING=OFF
cmake --build "${ssl_build}" --target ssl http ws http2 redis -j "${jobs}"
grep -E 'OpenSSL|OPENSSL' "${ssl_build}/CMakeCache.txt" >/dev/null
