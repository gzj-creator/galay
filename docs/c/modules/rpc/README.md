# RPC C ABI

`galay-rpc-c` exposes a C11 ABI for the existing RPC envelope plus a direct C coroutine TCP runtime. Public objects are opaque handles; create/acquire calls must be paired with destroy/release calls.

## Ownership

- `galay_rpc_client_create` / `galay_rpc_client_destroy` own one TCP client handle.
- `galay_rpc_server_create` / `galay_rpc_server_destroy` own the listener socket.
- `galay_rpc_service_create` / `galay_rpc_service_destroy` own method registration tables. Registered services must outlive the server using them.
- `galay_rpc_response_buffer_t.payload` is allocated by the C ABI and must be released with `galay_rpc_response_buffer_destroy`.
- Handler `request` payload pointers are borrowed and valid only during the callback. Handler response payload pointers are borrowed until the server sends the response before returning to the accept loop.

## Coroutine Semantics

`galay_rpc_client_connect`, `galay_rpc_client_call`, stream read/write, heartbeat, close, and `galay_rpc_server_serve_one` must run inside a `galay_coro_spawn` C coroutine. They suspend through the kernel C TCP ABI and return `C_IOResult`; they do not expose C++ `Task` types.

## Errors

RPC status is carried in `galay_rpc_error_code_t` and mapped by `galay_rpc_error_to_status`. Transport and coroutine failures return `C_IOResult` codes. Deadline timeout maps to `C_IOResultTimeout` and `GALAY_RPC_ERROR_DEADLINE_EXCEEDED`; pre-call cancellation maps to `C_IOResultCancelled` and `GALAY_RPC_ERROR_CANCELLED`.

## Runtime Surface

- Unary: register a service with `galay_rpc_service_register_unary`, then call `galay_rpc_client_call`.
- Streaming: `galay_rpc_client_stream_open`, `galay_rpc_stream_write`, `galay_rpc_stream_read`, and `galay_rpc_stream_close` provide frame-level streaming over one RPC request id.
- Heartbeat: `galay_rpc_client_heartbeat` sends a heartbeat frame and waits for the server echo.
- Pool: `galay_rpc_pool_*` is a logical lease pool matching the stable C++ connection-pool accounting surface; it does not own TCP sockets.

Current C runtime loopback tests cover unknown service, unknown method, unary success, bidi streaming, deadline timeout, cancellation, heartbeat, connection close, and pool lease replacement.
