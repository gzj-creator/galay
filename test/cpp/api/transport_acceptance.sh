#!/usr/bin/env bash
set -euo pipefail

if [[ "$1" == "startup" ]]; then
  exec cmake -DTEST_BINARY="$PWD/test/cpp/api/startup_server" \
    -DTEST_BINARY_DIR="$TEST_TMPDIR" -DTEST_WITH_SSL=OFF \
    -P test/cpp/api/startup_acceptance.cmake
fi

# The existing Bazel protocol targets do not enable the SSL feature.
exec node test/cpp/api/transport_acceptance.cjs test/cpp/api/transport_server \
  assets/swagger-ui "$TEST_TMPDIR/transport-evidence" http,h2c
