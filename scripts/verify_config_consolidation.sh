#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${root}/build/verify-config-consolidation"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"

if [[ -e "${root}/src/cpp/galay-etcd/async/config.h" ||
      -e "${root}/src/cpp/galay-mysql/async/config.h" ||
      -e "${root}/src/cpp/galay-mongo/async/config.h" ||
      -e "${root}/src/cpp/galay-redis/async/config.h" ||
      -e "${root}/src/cpp/galay-redis/base/redis_config.cc" ]]; then
  echo "obsolete config files still exist" >&2
  exit 1
fi

run_obsolete_scan() {
  local message="$1"
  local pattern="$2"
  local status
  shift 2

  set +e
  rg -n "${pattern}" "$@"
  status=$?
  set -e

  if [[ "${status}" -eq 0 ]]; then
    echo "${message}" >&2
    exit 1
  fi
  if [[ "${status}" -gt 1 ]]; then
    exit "${status}"
  fi
}

config_scan_paths=(
  "${root}/src/cpp/galay-etcd"
  "${root}/src/cpp/galay-mysql"
  "${root}/src/cpp/galay-mongo"
  "${root}/src/cpp/galay-redis"
  "${root}/test/etcd"
  "${root}/test/mysql"
  "${root}/test/mongo"
  "${root}/test/redis"
  "${root}/test/config"
  "${root}/examples/etcd"
  "${root}/examples/mysql"
  "${root}/examples/mongo"
  "${root}/examples/redis"
  "${root}/benchmark/etcd"
  "${root}/benchmark/mysql"
  "${root}/benchmark/mongo"
  "${root}/benchmark/redis"
  "${root}/docs/modules/etcd"
  "${root}/docs/modules/mysql"
  "${root}/docs/modules/mongo"
  "${root}/docs/modules/redis"
)
existing_config_paths=()
for path in "${config_scan_paths[@]}"; do
  if [[ -d "${path}" ]]; then
    existing_config_paths+=("${path}")
  fi
done

if [[ "${#existing_config_paths[@]}" -gt 0 ]]; then
  run_obsolete_scan \
    "obsolete config include or compatibility API remains" \
    'galay-etcd/async/(config|client_cfg)\.h|galay-mysql/async/config\.h|galay-mongo/async/config\.h|galay-redis/async/config\.h' \
    "${existing_config_paths[@]}"
fi

redis_scan_paths=(
  "${root}/src/cpp/galay-redis"
  "${root}/test/redis"
  "${root}/examples/redis"
  "${root}/benchmark/redis"
  "${root}/docs/modules/redis"
)
existing_redis_paths=()
for path in "${redis_scan_paths[@]}"; do
  if [[ -d "${path}" ]]; then
    existing_redis_paths+=("${path}")
  fi
done

if [[ "${#existing_redis_paths[@]}" -gt 0 ]]; then
  run_obsolete_scan \
    "obsolete config include or compatibility API remains" \
    'std::any|\bRedisConfig\b|RedisConnectionOption|getConnectOption|getParams' \
    "${existing_redis_paths[@]}"
fi

rm -rf "${build}"

cmake -S "${root}" -B "${build}" \
  -DGALAY_BUILD_UTILS=ON \
  -DGALAY_BUILD_KERNEL=ON \
  -DGALAY_BUILD_HTTP=ON \
  -DGALAY_BUILD_WS=OFF \
  -DGALAY_BUILD_HTTP2=OFF \
  -DGALAY_BUILD_REDIS=ON \
  -DGALAY_BUILD_ETCD=ON \
  -DGALAY_BUILD_MONGO=ON \
  -DGALAY_BUILD_MYSQL=ON \
  -DGALAY_BUILD_RPC=OFF \
  -DGALAY_BUILD_MCP=OFF \
  -DGALAY_BUILD_TRACING=OFF \
  -DGALAY_BUILD_SSL=OFF \
  -DBUILD_TESTING=ON

cmake --build "${build}" --target t_module_config_surface redis mongo mysql etcd -j "${jobs}"
ctest --test-dir "${build}" -R 'config.module_surface' --output-on-failure
