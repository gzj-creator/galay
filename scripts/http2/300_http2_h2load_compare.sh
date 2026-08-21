#!/usr/bin/env bash
set -euo pipefail

# h2load/nghttpd are external protocol tools, not the formal competitor. Keep
# the old command name as a non-ranking probe so automation fails closed.
printf '%s\n' \
  'status=not_applicable' \
  'competitor=boost.asio coroutine' \
  'scenario=http2 protocol' \
  'reason=No same-workload Boost.Asio coroutine HTTP/2 harness is registered; h2load/nghttpd data is historical/internal-only.'
