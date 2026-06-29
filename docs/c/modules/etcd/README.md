# Etcd C Async API

`galay-etcd-c` exposes an opaque C ABI for the Etcd HTTP v3 client surface. The C API uses explicit `galay_status_t` returns plus `galay_etcd_error_code_t` details; it does not expose C++ types or exceptions.

## Scope

- `galay_etcd_client_connect` opens a direct C coroutine TCP connection to `http://host:port`.
- KV calls support `/v3/kv/put`, `/v3/kv/range`, and `/v3/kv/deleterange` with Etcd base64 key/value JSON.
- Lease calls support grant, single keepalive, and revoke.
- Pipeline executes queued put/get/delete operations in order on the same connection and returns per-item results.
- Watch is pollable: create a watch handle, call `galay_etcd_watch_next`, then cancel or destroy it.
- Stats expose request/failure counters and lease keepalive counters for the single-endpoint path.

## Ownership

- Builders, clients, get results, pipelines, pipeline results, watches, and watch events are opaque handles.
- Every create/acquire has a matching destroy function.
- Pointers returned from result or event accessors are borrowed and remain valid only until the owning result/event is destroyed.

## Runtime And Errors

Network operations must be called from a Galay C coroutine because they suspend through `galay_kernel_tcp_socket_*`; they do not require an external Etcd service in tests or examples because local HTTP loopbacks are used.

Unsupported or failed operations return a `galay_status_t` and, when provided, set `galay_etcd_error_code_t`. Cancellation returns `GALAY_IO_ERROR` with `GALAY_ETCD_ERROR_CANCELLED`; using a closed client returns `GALAY_INVALID_ARGUMENT` with `GALAY_ETCD_ERROR_NOT_CONNECTED`.

## Limits

The current C ABI implements plain HTTP loopback behavior only. TLS Etcd endpoints and real multi-endpoint failover are not implemented in this agent scope; endpoint policy is accepted as configuration surface while stats cover the single-endpoint path.
