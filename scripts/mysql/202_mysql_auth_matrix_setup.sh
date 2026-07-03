#!/usr/bin/env bash
set -euo pipefail

admin_cmd=${MYSQL_AUTH_ADMIN_CMD:-mysql}
db=${GALAY_MYSQL_AUTH_DB:-galay_test}
password=${GALAY_MYSQL_AUTH_PASSWORD:-galay_auth_pass_123}
native_user=${GALAY_MYSQL_AUTH_NATIVE_USER:-galay_auth_native}
caching_user=${GALAY_MYSQL_AUTH_CACHING_USER:-galay_auth_caching}
sha256_user=${GALAY_MYSQL_AUTH_SHA256_USER:-galay_auth_sha256}
host=${GALAY_MYSQL_AUTH_USER_HOST:-127.0.0.1}

if [[ "${1:-}" == "--drop" ]]; then
  ${admin_cmd} <<SQL
DROP USER IF EXISTS '${native_user}'@'${host}';
DROP USER IF EXISTS '${caching_user}'@'${host}';
DROP USER IF EXISTS '${sha256_user}'@'${host}';
FLUSH PRIVILEGES;
SQL
  exit 0
fi

${admin_cmd} <<SQL
CREATE DATABASE IF NOT EXISTS ${db};

DROP USER IF EXISTS '${native_user}'@'${host}';
DROP USER IF EXISTS '${caching_user}'@'${host}';
DROP USER IF EXISTS '${sha256_user}'@'${host}';

CREATE USER '${native_user}'@'${host}'
  IDENTIFIED WITH mysql_native_password BY '${password}';
CREATE USER '${caching_user}'@'${host}'
  IDENTIFIED WITH caching_sha2_password BY '${password}';
CREATE USER '${sha256_user}'@'${host}'
  IDENTIFIED WITH sha256_password BY '${password}';

GRANT ALL PRIVILEGES ON ${db}.* TO '${native_user}'@'${host}';
GRANT ALL PRIVILEGES ON ${db}.* TO '${caching_user}'@'${host}';
GRANT ALL PRIVILEGES ON ${db}.* TO '${sha256_user}'@'${host}';
FLUSH PRIVILEGES;
SQL
