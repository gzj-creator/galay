#!/usr/bin/env bash
set -euo pipefail

build_dir=${GALAY_BUILD_DIR:-build/client-prod-verify}
base_dir=${GALAY_ETCD_VERIFY_DIR:-$(mktemp -d /tmp/galay-etcd.XXXXXX)}
endpoint=${GALAY_ETCD_ENDPOINT:-http://127.0.0.1:12379}
pids=()

run()
{
  printf '+ %s\n' "$*"
  "$@"
}

cleanup()
{
  for pid in "${pids[@]:-}"; do
    if kill -0 "${pid}" >/dev/null 2>&1; then
      kill "${pid}" >/dev/null 2>&1 || true
    fi
  done
  for pid in "${pids[@]:-}"; do
    wait "${pid}" >/dev/null 2>&1 || true
  done
  rm -rf "${base_dir}"
}
trap cleanup EXIT

if [[ ! -d "${build_dir}" ]]; then
  echo "build dir not found: ${build_dir}" >&2
  exit 1
fi

run cmake --build "${build_dir}" --target \
  etcd_t1_smoke etcd_t2_prefix etcd_t3_pipe etcd_t4_smoke etcd_t5_pipe etcd_t8_task_watch \
  etcd_t13_cluster_integration etcd_t14_async_cluster_integration

mkdir -p "${base_dir}"

initial_cluster='n1=http://127.0.0.1:12380,n2=http://127.0.0.1:12382,n3=http://127.0.0.1:12384'
cluster_endpoints='http://127.0.0.1:12379,http://127.0.0.1:12381,http://127.0.0.1:12383'

start_node()
{
  local name=$1
  local client_port=$2
  local peer_port=$3
  local data_dir="${base_dir}/${name}"
  mkdir -p "${data_dir}"
  etcd \
    --name "${name}" \
    --data-dir "${data_dir}" \
    --listen-client-urls "http://127.0.0.1:${client_port}" \
    --advertise-client-urls "http://127.0.0.1:${client_port}" \
    --listen-peer-urls "http://127.0.0.1:${peer_port}" \
    --initial-advertise-peer-urls "http://127.0.0.1:${peer_port}" \
    --initial-cluster "${initial_cluster}" \
    --initial-cluster-state new \
    --initial-cluster-token galay-etcd-verify \
    >"${base_dir}/${name}.log" 2>&1 &
  pids+=("$!")
}

start_node n1 12379 12380
start_node n2 12381 12382
start_node n3 12383 12384

for _ in $(seq 1 80); do
  if ETCDCTL_API=3 etcdctl --endpoints="${endpoint}" endpoint health >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done

run env ETCDCTL_API=3 etcdctl --endpoints="${endpoint}" endpoint status --write-out=table
run env ETCDCTL_API=3 etcdctl --endpoints=http://127.0.0.1:12379,http://127.0.0.1:12381,http://127.0.0.1:12383 endpoint health

run env GALAY_IT_ENABLE=1 "${build_dir}/test/cpp/etcd/etcd_t1_smoke" "${endpoint}"
run env GALAY_IT_ENABLE=1 "${build_dir}/test/cpp/etcd/etcd_t2_prefix" "${endpoint}"
run env GALAY_IT_ENABLE=1 "${build_dir}/test/cpp/etcd/etcd_t3_pipe" "${endpoint}"
run env GALAY_IT_ENABLE=1 "${build_dir}/test/cpp/etcd/etcd_t4_smoke" "${endpoint}"
run env GALAY_IT_ENABLE=1 "${build_dir}/test/cpp/etcd/etcd_t5_pipe" "${endpoint}"
run env GALAY_IT_ENABLE=1 "${build_dir}/test/cpp/etcd/etcd_t8_task_watch" "${endpoint}"
run env GALAY_IT_ENABLE=1 GALAY_ETCD_ENDPOINTS="${cluster_endpoints}" \
  "${build_dir}/test/cpp/etcd/etcd_t13_cluster_integration"
run env GALAY_IT_ENABLE=1 GALAY_ETCD_ENDPOINTS="${cluster_endpoints}" \
  "${build_dir}/test/cpp/etcd/etcd_t14_async_cluster_integration"

sync_example="${build_dir}/examples/cpp/etcd/example_etcd_include_sync_basic"
async_example="${build_dir}/examples/cpp/etcd/example_etcd_include_async_basic"
sync_example_available=0

if [[ -f "${sync_example}" ]]; then
  run "${sync_example}" "${endpoint}"
  sync_example_available=1
else
  echo "[SKIP] examples disabled or not built: ${sync_example}"
fi

if [[ -f "${async_example}" ]]; then
  run "${async_example}" "${endpoint}"
else
  echo "[SKIP] examples disabled or not built: ${async_example}"
fi

if [[ "${sync_example_available}" -eq 1 ]]; then
  if "${sync_example}" "https://127.0.0.1:12379"; then
    echo "https etcd endpoint unexpectedly succeeded" >&2
    exit 1
  else
    echo "etcd https endpoint rejected as expected"
  fi
fi

echo "etcd temporary 3-node HTTP cluster verification completed"
