#!/usr/bin/env bash
set -euo pipefail

# hiredis and redis-benchmark are historical fixtures only. Do not publish
# their numbers as a competitor ranking without a same-workload Boost.Asio
# coroutine Redis protocol harness.
printf '%s\n' \
  'status=not_applicable' \
  'competitor=boost.asio coroutine' \
  'scenario=redis client protocol' \
  'reason=No same-workload Boost.Asio coroutine Redis protocol harness is registered; old client-library data is historical/internal-only.'
