#!/usr/bin/env bash
set -euo pipefail

# etcdctl is an external client/tool benchmark, not a Boost.Asio coroutine
# baseline. Keep this entry point deterministic for callers while preventing
# stale third-party numbers from being published as a competitor ranking.
printf '%s\n' \
  'status=not_applicable' \
  'competitor=boost.asio coroutine' \
  'scenario=etcd client protocol' \
  'reason=No same-workload Boost.Asio coroutine etcd protocol harness is registered; external etcdctl data is historical/internal-only.'
