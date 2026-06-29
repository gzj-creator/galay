# Redis C API

`src/c/galay-redis-c/redis.h` exposes the Redis C ABI. The surface is pure C:
opaque handles, `galay_status_t`, `C_IOResult`, and borrowed pointers with explicit
destroy/release functions.

## RESP Values

`galay_redis_command_builder_create`, `galay_redis_command_builder_build`, and
`galay_redis_command_builder_destroy` encode command names and argument arrays into
RESP command bytes. The returned encoded buffer is borrowed from the builder and
remains valid until the next build or destroy.

`galay_redis_parse_reply` parses RESP2 replies and the RESP3 types currently exposed
by the C ABI: nil, double, boolean, blob error, verbatim string, big number, map,
set, and push. Replies are owned by the caller and must be released with
`galay_redis_reply_free` or `galay_redis_reply_destroy`.

String accessors return borrowed memory owned by the reply. Array, set, push, and
map child accessors also return borrowed child pointers; free only the root reply.
Scalar type mismatches return `GALAY_INVALID_ARGUMENT`, and malformed RESP returns
`GALAY_PROTOCOL_ERROR`.

## Standalone Client And Pipeline

`galay_redis_client_connect`, `galay_redis_client_command_async`,
`galay_redis_client_auth`, `galay_redis_client_select`,
`galay_redis_client_pipeline_async`, and `galay_redis_client_close` must be called
from a Galay C coroutine. These calls suspend through the kernel C TCP socket API
and do not block the calling thread. Pipeline replies are returned as an owned array
and must be released with `galay_redis_pipeline_replies_destroy`.

`galay_redis_client_command` and `galay_redis_client_disconnect` are synchronous
compatibility helpers for already-created clients; coroutine users should prefer
the async calls above.

## Pool Lease Ownership

`galay_redis_pool_create` copies the client configuration into the pool. Acquire
returns a `galay_redis_pool_lease_t`; the borrowed `galay_redis_client_t*` from
`galay_redis_pool_lease_client` remains valid until `galay_redis_pool_release`.
The pool is intentionally same-runtime/same-thread; it does not add cross-thread
synchronization around the underlying C client handles.

If the pool is exhausted, acquire returns `C_IOResultTimeout`. Connection failures
from the underlying client are propagated as `C_IOResult`. Destroying the pool
destroys its client handles, so callers must release leases before destroy.

## Cluster Route ABI

`galay_redis_cluster_key_slot` implements Redis Cluster CRC16 and `{hash-tag}`
rules. `galay_redis_cluster_add_node`, `galay_redis_cluster_route_key`, and
`galay_redis_cluster_route_slot` provide an offline route table for local routing
decisions. `galay_redis_cluster_apply_redirect` parses `-MOVED` and `-ASK` error
replies. MOVED updates the slot cache; ASK returns a temporary route without
mutating the cache.

Route `host` pointers are borrowed from the cluster and remain valid until the next
cluster mutation or destroy.

## TLS / Rediss

Rediss/TLS is not exposed in this C ABI yet. It should be added after the SSL C
async socket integration lands, instead of pretending a plaintext socket provides
TLS semantics.
