#!/usr/bin/env bash
set -euo pipefail

build_dir=${GALAY_BUILD_DIR:-build/client-prod-verify}
mongo_host=${GALAY_MONGO_HOST:-127.0.0.1}
mongo_port=${GALAY_MONGO_PORT:-27017}
mongo_db=${GALAY_MONGO_DB:-galay_test}
mongo_user=${GALAY_MONGO_USER:-galay_scram_user}
mongo_password=${GALAY_MONGO_PASSWORD:-galay_scram_pass_123}
mongo_auth_db=${GALAY_MONGO_AUTH_DB:-admin}
mongod_conf=${GALAY_MONGOD_CONF:-/etc/mongod.conf}
backup_conf=
restored=0

run()
{
  printf '+ %s\n' "$*"
  "$@"
}

wait_mongo_ping()
{
  for _ in $(seq 1 80); do
    if mongosh --quiet --host "${mongo_host}" --port "${mongo_port}" \
      --eval 'db.adminCommand({ping:1})' >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.25
  done
  echo "mongod did not become ready" >&2
  return 1
}

wait_mongo_auth_ping()
{
  for _ in $(seq 1 80); do
    if mongosh --quiet --host "${mongo_host}" --port "${mongo_port}" \
      -u "${mongo_user}" -p "${mongo_password}" --authenticationDatabase "${mongo_auth_db}" \
      --eval 'db.adminCommand({ping:1})' >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.25
  done
  echo "mongod auth ping did not become ready" >&2
  return 1
}

restore_config()
{
  if [[ "${restored}" == "1" ]]; then
    return
  fi
  restored=1
  if [[ -n "${backup_conf}" && -f "${backup_conf}" ]]; then
    sudo cp "${backup_conf}" "${mongod_conf}"
    sudo systemctl restart mongod
    rm -f "${backup_conf}"
  fi
}

trap restore_config EXIT

if [[ ! -d "${build_dir}" ]]; then
  echo "build dir not found: ${build_dir}" >&2
  exit 1
fi

run cmake --build "${build_dir}" --target \
  mongo_t2_client mongo_t3_pipeline mongo_t4_func mongo_t5_func mongo_t6_auth mongo_t7_bridge

run mongosh --quiet --host "${mongo_host}" --port "${mongo_port}" --eval 'db.adminCommand({ping:1})'

run mongosh --quiet --host "${mongo_host}" --port "${mongo_port}" --eval "
db.getSiblingDB('${mongo_db}').dropDatabase();
db = db.getSiblingDB('${mongo_auth_db}');
try {
  db.dropUser('${mongo_user}');
} catch (e) {
  if (e.codeName !== 'UserNotFound') {
    throw e;
  }
}
db.createUser({
  user: '${mongo_user}',
  pwd: '${mongo_password}',
  roles: [
    { role: 'readWrite', db: '${mongo_db}' },
    { role: 'read', db: '${mongo_auth_db}' }
  ],
  mechanisms: ['SCRAM-SHA-256']
});
"

backup_conf=$(mktemp /tmp/galay-mongod.conf.XXXXXX)
sudo cp "${mongod_conf}" "${backup_conf}"

sudo python3 - "${mongod_conf}" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
lines = text.splitlines()
out = []
i = 0
while i < len(lines):
    line = lines[i]
    if line.strip() == "security:":
        out.append("security:")
        i += 1
        while i < len(lines) and (lines[i].startswith(" ") or lines[i].startswith("\t") or lines[i].strip() == ""):
            if lines[i].strip().startswith("authorization:"):
                i += 1
                continue
            out.append(lines[i])
            i += 1
        out.append("  authorization: enabled")
        continue
    out.append(line)
    i += 1

if not any(line.strip() == "security:" for line in out):
    if out and out[-1].strip() != "":
        out.append("")
    out.append("security:")
    out.append("  authorization: enabled")

path.write_text("\n".join(out) + "\n")
PY

run sudo systemctl restart mongod
wait_mongo_auth_ping
run mongosh --quiet --host "${mongo_host}" --port "${mongo_port}" \
  -u "${mongo_user}" -p "${mongo_password}" --authenticationDatabase "${mongo_auth_db}" \
  --eval 'db.adminCommand({ping:1})'

run env \
  GALAY_MONGO_HOST="${mongo_host}" \
  GALAY_MONGO_PORT="${mongo_port}" \
  GALAY_MONGO_DB="${mongo_db}" \
  GALAY_MONGO_USER="${mongo_user}" \
  GALAY_MONGO_PASSWORD="${mongo_password}" \
  GALAY_MONGO_AUTH_DB="${mongo_auth_db}" \
  "${build_dir}/test/mongo/mongo_t2_client"
run env \
  GALAY_MONGO_HOST="${mongo_host}" \
  GALAY_MONGO_PORT="${mongo_port}" \
  GALAY_MONGO_DB="${mongo_db}" \
  GALAY_MONGO_USER="${mongo_user}" \
  GALAY_MONGO_PASSWORD="${mongo_password}" \
  GALAY_MONGO_AUTH_DB="${mongo_auth_db}" \
  "${build_dir}/test/mongo/mongo_t3_pipeline"
run env \
  GALAY_MONGO_HOST="${mongo_host}" \
  GALAY_MONGO_PORT="${mongo_port}" \
  GALAY_MONGO_DB="${mongo_db}" \
  GALAY_MONGO_USER="${mongo_user}" \
  GALAY_MONGO_PASSWORD="${mongo_password}" \
  GALAY_MONGO_AUTH_DB="${mongo_auth_db}" \
  "${build_dir}/test/mongo/mongo_t4_func"
run env \
  GALAY_MONGO_HOST="${mongo_host}" \
  GALAY_MONGO_PORT="${mongo_port}" \
  GALAY_MONGO_DB="${mongo_db}" \
  GALAY_MONGO_USER="${mongo_user}" \
  GALAY_MONGO_PASSWORD="${mongo_password}" \
  GALAY_MONGO_AUTH_DB="${mongo_auth_db}" \
  "${build_dir}/test/mongo/mongo_t5_func"
run env \
  GALAY_MONGO_HOST="${mongo_host}" \
  GALAY_MONGO_PORT="${mongo_port}" \
  GALAY_MONGO_DB="${mongo_db}" \
  GALAY_MONGO_USER="${mongo_user}" \
  GALAY_MONGO_PASSWORD="${mongo_password}" \
  GALAY_MONGO_AUTH_DB="${mongo_auth_db}" \
  "${build_dir}/test/mongo/mongo_t6_auth"
run env \
  GALAY_MONGO_HOST="${mongo_host}" \
  GALAY_MONGO_PORT="${mongo_port}" \
  GALAY_MONGO_DB="${mongo_db}" \
  GALAY_MONGO_USER="${mongo_user}" \
  GALAY_MONGO_PASSWORD="${mongo_password}" \
  GALAY_MONGO_AUTH_DB="${mongo_auth_db}" \
  "${build_dir}/test/mongo/mongo_t7_bridge"

run env \
  GALAY_MONGO_HOST="${mongo_host}" \
  GALAY_MONGO_PORT="${mongo_port}" \
  GALAY_MONGO_DB="${mongo_db}" \
  GALAY_MONGO_USER="${mongo_user}" \
  GALAY_MONGO_PASSWORD="${mongo_password}" \
  GALAY_MONGO_AUTH_DB="${mongo_auth_db}" \
  "${build_dir}/examples/mongo/example_mongo_include_sync_ping"
run env \
  GALAY_MONGO_HOST="${mongo_host}" \
  GALAY_MONGO_PORT="${mongo_port}" \
  GALAY_MONGO_DB="${mongo_db}" \
  GALAY_MONGO_USER="${mongo_user}" \
  GALAY_MONGO_PASSWORD="${mongo_password}" \
  GALAY_MONGO_AUTH_DB="${mongo_auth_db}" \
  "${build_dir}/examples/mongo/example_mongo_include_async_ping"

if env \
  GALAY_MONGO_HOST="${mongo_host}" \
  GALAY_MONGO_PORT="${mongo_port}" \
  GALAY_MONGO_DB="${mongo_db}" \
  GALAY_MONGO_USER="${mongo_user}" \
  GALAY_MONGO_PASSWORD="${mongo_password}_wrong" \
  GALAY_MONGO_AUTH_DB="${mongo_auth_db}" \
  "${build_dir}/test/mongo/mongo_t6_auth"; then
  echo "wrong Mongo password unexpectedly succeeded" >&2
  exit 1
else
  echo "Mongo wrong-password negative case failed as expected"
fi

restore_config
wait_mongo_ping
run mongosh --quiet --host "${mongo_host}" --port "${mongo_port}" --eval 'db.adminCommand({ping:1})'
echo "Mongo auth verification completed and mongod config restored"
