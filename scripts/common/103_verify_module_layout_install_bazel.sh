#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
build="${root}/build/verify-module-layout-install"
prefix="${build}/install"
consumer="${build}/consumer"
consumer_build="${build}/consumer-build"

cd "${root}"

required_dirs=(
  "src/cpp/galay-kernel/core"
  "src/cpp/galay-http/protoc"
  "src/cpp/galay-http2/protoc"
  "src/cpp/galay-ws/protoc"
  "src/cpp/galay-rpc/protoc"
  "src/cpp/galay-redis/protoc"
  "src/cpp/galay-mysql/protoc"
  "src/cpp/galay-mongo/protoc"
)
for dir in "${required_dirs[@]}"; do
  if [[ ! -d "${dir}" ]]; then
    echo "required module layout directory is missing: ${dir}" >&2
    exit 1
  fi
done

if [[ -d "${root}/src/cpp/galay-tracing/internal" ]] && ! find "${root}/src/cpp/galay-tracing/internal" -type f | grep -q .; then
  echo "empty accidental source directory remains: src/cpp/galay-tracing/internal" >&2
  exit 1
fi

active_scan_paths=(
  src
  test/etcd
  test/http
  test/http2
  test/kernel
  test/mongo
  test/mysql
  test/redis
  test/rpc
  test/ssl
  test/tracing
  test/utils
  test/ws
  examples
  benchmark
  CMakeLists.txt
  cmake
)

if rg -n 'http/protocol|http2/protocol|ws/protocol|rpc/protocol|redis/protocol|mysql/protocol|mongo/protocol|src/(utils|kernel|http|http2|ws|rpc|redis|mysql|mongo)(/|\b)|<(utils|kernel|http|http2|ws|rpc|redis|mysql|mongo)/' \
  "${active_scan_paths[@]}" 2>/dev/null; then
  echo "stale protocol layout references remain" >&2
  exit 1
fi

rm -rf "${build}"

cmake -S "${root}" -B "${build}" \
  -DGALAY_BUILD_UTILS=ON \
  -DGALAY_BUILD_KERNEL=ON \
  -DGALAY_BUILD_SSL=OFF \
  -DGALAY_BUILD_HTTP=ON \
  -DGALAY_BUILD_WS=ON \
  -DGALAY_BUILD_HTTP2=ON \
  -DGALAY_BUILD_REDIS=ON \
  -DGALAY_BUILD_RPC=ON \
  -DGALAY_BUILD_MYSQL=OFF \
  -DGALAY_BUILD_MONGO=OFF \
  -DGALAY_BUILD_ETCD=OFF \
  -DGALAY_BUILD_MCP=OFF \
  -DGALAY_BUILD_TRACING=OFF \
  -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX="${prefix}"

cmake --build "${build}" --target galay-utils galay-kernel galay-http galay-ws galay-http2 galay-redis galay-rpc -j "${jobs}"
cmake --install "${build}"

test -f "${prefix}/lib/cmake/galay/galayConfig.cmake"
test -f "${prefix}/lib/cmake/galay/galayTargets.cmake"
test -f "${prefix}/lib/cmake/galay-kernel/galay-kernelConfig.cmake"
test -f "${prefix}/include/galay/cpp/galay-kernel/core/runtime.h"
test -f "${prefix}/include/galay/cpp/galay-http/protoc/http_request.h"
test -f "${prefix}/include/galay/cpp/galay-ws/protoc/ws_frame.h"
test -f "${prefix}/include/galay/cpp/galay-rpc/protoc/rpc_message.h"
test -f "${prefix}/include/galay/cpp/galay-redis/protoc/redis_protocol.h"

mkdir -p "${consumer}"
cat > "${consumer}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(galay_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(galay CONFIG REQUIRED)
add_executable(galay_consumer main.cc)
set_target_properties(galay_consumer PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
target_include_directories(galay_consumer BEFORE PRIVATE "${CMAKE_PREFIX_PATH}/include")
target_link_libraries(galay_consumer PRIVATE galay::kernel galay::http galay::redis)
EOF

cat > "${consumer}/main.cc" <<'EOF'
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-redis/base/redis_config.h>
#include <galay/cpp/galay-redis/protoc/redis_protocol.h>

int main()
{
    galay::redis::RedisSessionConfig cfg;
    cfg.host = "127.0.0.1";
    galay::http::HttpRequest request;
    return cfg.host.empty() ? 1 : 0;
}
EOF

cmake -S "${consumer}" -B "${consumer_build}" -DCMAKE_PREFIX_PATH="${prefix}"
cmake --build "${consumer_build}" -j "${jobs}"

test -f MODULE.bazel
test -f BUILD.bazel
test -f src/cpp/galay-utils/BUILD
test -f src/cpp/galay-kernel/BUILD
test -f src/cpp/galay-http/BUILD

if command -v bazel >/dev/null 2>&1; then
  bazel query //src/... >/dev/null
  bazel build //src/cpp/galay-utils:galay-utils //src/cpp/galay-kernel:galay-kernel >/dev/null
fi
