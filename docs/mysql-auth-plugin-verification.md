# MySQL auth plugin verification

This repo includes a real-server MySQL auth plugin integration test:
`mysql.auth_plugins`.

The test is skipped unless explicit credentials are provided. This keeps
ordinary unit-test runs independent of local machine secrets while allowing CI
or a production-like host to exercise real MySQL authentication behavior.

## Coverage

`mysql.auth_plugins` validates these paths against a real MySQL server:

- `mysql_native_password` sync connect, query, ping, close, reconnect, and wrong-password failure.
- `caching_sha2_password` sync connect, query, ping, close, reconnect, and wrong-password failure.
- `mysql_native_password` async connect and query.
- `caching_sha2_password` async connect and query.
- `sha256_password` unsupported-plugin failure for sync and async clients.

`mysql.resilience` validates these real-server recovery paths:

- Connection failure to a refused TCP port, followed by successful connection to the real server.
- Server-side `KILL CONNECTION` on an active client connection, followed by error propagation and a fresh reconnect.

`caching_sha2_password` is tested with TLS disabled at the MySQL CLI level when
checking the host setup, so the server has RSA keys available for non-TLS auth.
The Galay MySQL client does not currently expose a MySQL `CLIENT_SSL` handshake
path, so end-to-end MySQL TLS is intentionally not covered by this test.

## Local MySQL setup

Create disposable users on a MySQL 8 server that has the auth plugins enabled.
Use an administrative account appropriate for the host.

```sh
MYSQL_AUTH_ADMIN_CMD="sudo mysql --defaults-file=/etc/mysql/debian.cnf" \
GALAY_MYSQL_AUTH_DB=galay_test \
GALAY_MYSQL_AUTH_PASSWORD=galay_auth_pass_123 \
bash scripts/mysql/mysql_auth_matrix_setup.sh
```

Run the integration test with the credentials injected through the environment:

```sh
env \
  GALAY_MYSQL_AUTH_HOST=127.0.0.1 \
  GALAY_MYSQL_AUTH_PORT=3306 \
  GALAY_MYSQL_AUTH_DB=galay_test \
  GALAY_MYSQL_AUTH_PASSWORD=galay_auth_pass_123 \
  ctest --test-dir build/client-prod-verify --output-on-failure -R 'mysql\.auth_plugins'
```

Run the resilience test with a regular user that can connect to the test
database and kill its own connection:

```sh
env \
  GALAY_MYSQL_HOST=127.0.0.1 \
  GALAY_MYSQL_PORT=3306 \
  GALAY_MYSQL_USER=galay \
  GALAY_MYSQL_PASSWORD=galay_pass \
  GALAY_MYSQL_DB=galay_test \
  ctest --test-dir build/client-prod-verify --output-on-failure -R 'mysql\.resilience'
```

## Cleanup

```sh
MYSQL_AUTH_ADMIN_CMD="sudo mysql --defaults-file=/etc/mysql/debian.cnf" \
bash scripts/mysql/mysql_auth_matrix_setup.sh --drop
```
