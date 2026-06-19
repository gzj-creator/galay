#!/usr/bin/env bash
set -euo pipefail

build_dir=${GALAY_BUILD_DIR:-build/client-prod-verify}
base_dir=${GALAY_REDIS_VERIFY_DIR:-$(mktemp -d /tmp/galay-redis.XXXXXX)}
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

run cmake --build "${build_dir}" --target redis_t27_auth redis_t17_tls redis_t18_url redis_t19_topology redis_t13_cluster
mkdir -p "${base_dir}"

wait_redis()
{
  local port=$1
  shift || true
  for _ in $(seq 1 80); do
    if redis-cli -h 127.0.0.1 -p "${port}" "$@" PING >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.25
  done
  echo "redis on port ${port} did not become ready" >&2
  return 1
}

start_redis()
{
  local conf=$1
  redis-server "${conf}" >"${conf}.log" 2>&1 &
  pids+=("$!")
}

acl_dir="${base_dir}/acl"
mkdir -p "${acl_dir}"
cat >"${acl_dir}/redis.conf" <<CONF
bind 127.0.0.1
port 16379
protected-mode no
daemonize no
dir ${acl_dir}
save ""
appendonly no
user default off
user galay on >galay_redis_pass_123 ~* &* +@all
CONF
start_redis "${acl_dir}/redis.conf"
wait_redis 16379 --user galay -a galay_redis_pass_123
run redis-cli -h 127.0.0.1 -p 16379 --user galay -a galay_redis_pass_123 ACL WHOAMI
run env \
  GALAY_REDIS_AUTH_URL=redis://galay:galay_redis_pass_123@127.0.0.1:16379/0 \
  GALAY_REDIS_AUTH_WRONG_URL=redis://galay:bad_password@127.0.0.1:16379/0 \
  "${build_dir}/test/redis/redis_t27_auth"

tls_dir="${base_dir}/tls"
mkdir -p "${tls_dir}"
run openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj "/CN=localhost" \
  -keyout "${tls_dir}/ca.key" \
  -out "${tls_dir}/ca.crt"
run openssl req -newkey rsa:2048 -nodes \
  -subj "/CN=localhost" \
  -keyout "${tls_dir}/server.key" \
  -out "${tls_dir}/server.csr"
cat >"${tls_dir}/server.ext" <<CONF
subjectAltName=DNS:localhost,IP:127.0.0.1
CONF
run openssl x509 -req -days 1 \
  -in "${tls_dir}/server.csr" \
  -CA "${tls_dir}/ca.crt" \
  -CAkey "${tls_dir}/ca.key" \
  -CAcreateserial \
  -extfile "${tls_dir}/server.ext" \
  -out "${tls_dir}/server.crt"
cat >"${tls_dir}/redis.conf" <<CONF
bind 127.0.0.1
port 0
tls-port 16380
protected-mode no
daemonize no
dir ${tls_dir}
save ""
appendonly no
tls-cert-file ${tls_dir}/server.crt
tls-key-file ${tls_dir}/server.key
tls-ca-cert-file ${tls_dir}/ca.crt
tls-auth-clients no
CONF
start_redis "${tls_dir}/redis.conf"
for _ in $(seq 1 80); do
  if redis-cli --tls --cacert "${tls_dir}/ca.crt" -h localhost -p 16380 PING >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done
run redis-cli --tls --cacert "${tls_dir}/ca.crt" -h localhost -p 16380 PING
run env \
  GALAY_REDIS_TLS_URL=rediss://localhost:16380/0 \
  GALAY_REDIS_TLS_LOCALHOST_URL=rediss://localhost:16380/0 \
  GALAY_REDIS_TLS_CA="${tls_dir}/ca.crt" \
  GALAY_REDIS_TLS_VERIFY_PEER=1 \
  GALAY_REDIS_TLS_SERVER_NAME=localhost \
  "${build_dir}/test/redis/redis_t17_tls"
run env \
  GALAY_REDIS_TLS_URL=rediss://localhost:16380/0 \
  GALAY_REDIS_TLS_LOCALHOST_URL=rediss://localhost:16380/0 \
  GALAY_REDIS_TLS_CA="${tls_dir}/ca.crt" \
  GALAY_REDIS_TLS_VERIFY_PEER=1 \
  GALAY_REDIS_TLS_SERVER_NAME=localhost \
  "${build_dir}/test/redis/redis_t18_url"
run env \
  GALAY_REDIS_TLS_URL=rediss://localhost:16380/0 \
  GALAY_REDIS_TLS_CA="${tls_dir}/ca.crt" \
  GALAY_REDIS_TLS_VERIFY_PEER=1 \
  GALAY_REDIS_TLS_SERVER_NAME=localhost \
  "${build_dir}/test/redis/redis_t19_topology"

cluster_dir="${base_dir}/cluster"
mkdir -p "${cluster_dir}"
for port in 17000 17001 17002; do
  node_dir="${cluster_dir}/${port}"
  mkdir -p "${node_dir}"
  bus_port=$((port + 10000))
  cat >"${node_dir}/redis.conf" <<CONF
bind 127.0.0.1
port ${port}
protected-mode no
daemonize no
dir ${node_dir}
save ""
appendonly no
cluster-enabled yes
cluster-config-file nodes.conf
cluster-node-timeout 5000
cluster-announce-ip 127.0.0.1
cluster-announce-port ${port}
cluster-announce-bus-port ${bus_port}
CONF
  start_redis "${node_dir}/redis.conf"
done
wait_redis 17000
wait_redis 17001
wait_redis 17002
redis-cli --cluster create \
  127.0.0.1:17000 \
  127.0.0.1:17001 \
  127.0.0.1:17002 \
  --cluster-yes >/tmp/galay-redis-cluster-create.log
run redis-cli -h 127.0.0.1 -p 17000 CLUSTER INFO

sentinel_dir="${base_dir}/sentinel"
mkdir -p "${sentinel_dir}"
for item in "17100 master" "17101 replica1" "17102 replica2"; do
  set -- ${item}
  port=$1
  name=$2
  node_dir="${sentinel_dir}/${name}"
  mkdir -p "${node_dir}"
  cat >"${node_dir}/redis.conf" <<CONF
bind 127.0.0.1
port ${port}
protected-mode no
daemonize no
dir ${node_dir}
save ""
appendonly no
CONF
  start_redis "${node_dir}/redis.conf"
done
wait_redis 17100
wait_redis 17101
wait_redis 17102
run redis-cli -h 127.0.0.1 -p 17101 REPLICAOF 127.0.0.1 17100
run redis-cli -h 127.0.0.1 -p 17102 REPLICAOF 127.0.0.1 17100

cat >"${sentinel_dir}/sentinel.conf" <<CONF
bind 127.0.0.1
port 26380
protected-mode no
daemonize no
dir ${sentinel_dir}
sentinel monitor mymaster 127.0.0.1 17100 1
sentinel down-after-milliseconds mymaster 1000
sentinel failover-timeout mymaster 10000
sentinel parallel-syncs mymaster 1
CONF
redis-server "${sentinel_dir}/sentinel.conf" --sentinel >"${sentinel_dir}/sentinel.log" 2>&1 &
pids+=("$!")
wait_redis 26380
run redis-cli -h 127.0.0.1 -p 26380 SENTINEL get-master-addr-by-name mymaster

run env \
  GALAY_IT_ENABLE=1 \
  GALAY_IT_CLUSTER_HOST=127.0.0.1 \
  GALAY_IT_CLUSTER_PORT=17000 \
  GALAY_IT_SENTINEL_HOST=127.0.0.1 \
  GALAY_IT_SENTINEL_PORT=26380 \
  GALAY_IT_SENTINEL_MASTER_NAME=mymaster \
  GALAY_IT_TRIGGER_SENTINEL_FAILOVER=1 \
  "${build_dir}/test/redis/redis_t13_cluster"

echo "Redis ACL/TLS/cluster/sentinel verification completed"
