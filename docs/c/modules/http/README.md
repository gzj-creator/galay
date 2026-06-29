# HTTP C API

`galay-c-http` exposes a minimal HTTP/1.1 C runtime surface backed by the kernel direct C coroutine TCP APIs.

## Scope

- Opaque handles: `galay_http_client_t`, `galay_http_server_t`, `galay_http_session_t`.
- Request/response ownership: create/recv functions allocate request or response objects; callers release them with `galay_http_request_destroy` or `galay_http_response_destroy`.
- Direct coroutine I/O: connect, accept, send, recv, close, and `serve_one` must run inside a `galay_coro_spawn` C coroutine on a running kernel runtime.
- Route callbacks: `galay_http_server_add_route` registers exact method/path handlers. `serve_one` accepts one connection, reads one request, invokes the handler, sends one response, and closes the session.
- Streaming smoke surface: `galay_http_session_send_bytes` and `galay_http_session_recv_bytes` expose raw session bytes for split-response, timeout, malformed, and closed-peer tests.

## Error Model

Synchronous create/bind/listen/register functions return `galay_status_t`; use `galay_http_get_error` for readable text. Suspending I/O returns `C_IOResult` from the kernel C coroutine ABI:

- `C_IOResultOk`: operation completed.
- `C_IOResultTimeout`: timeout expired before completion.
- `C_IOResultEof`: peer closed or zero-byte transfer.
- `C_IOResultInvalid`: invalid handle, invalid state, or call outside a C coroutine.
- `C_IOResultError`: protocol, allocation, or kernel I/O failure.

The implementation does not add C++ `try`/`catch`/`throw` paths.

## Deferred Areas

This C layer intentionally does not claim full C++ HTTP parity yet. TLS/HTTPS, keep-alive connection loops, chunked transfer decoding, static file/proxy helpers, router patterns, and HTTP/2 remain C++-only or separate module work.
