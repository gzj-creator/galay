#!/usr/bin/env bash
set -euo pipefail

# libmysqlclient and mysqlslap are retained as historical fixtures only. This
# entry point must not execute or rank them under the formal comparison policy.
printf '%s\n' \
  'status=not_applicable' \
  'competitor=boost.asio coroutine' \
  'scenario=mysql client protocol' \
  'reason=No same-workload Boost.Asio coroutine MySQL protocol harness is registered; old client-library data is historical/internal-only.'
