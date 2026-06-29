# HTTP C API

`galay-c-http` exposes a minimal HTTP/1.1 C runtime surface backed by the kernel direct C coroutine TCP APIs.

## Scope

- Opaque handles: `galay_http_client_t`, `galay_http_server_t`, `galay_http_session_t`.
- Header builder/accessor: `galay_http_headers_create`, `galay_http_headers_add`,
  `galay_http_headers_find`, and `galay_http_headers_remove` manage standalone
  header lists.
- Request/response ownership: create/recv functions allocate request or response objects; callers release them with `galay_http_request_destroy` or `galay_http_response_destroy`.
- Direct coroutine I/O: connect, accept, send, recv, close, and `serve_one` must run inside a `galay_coro_spawn` C coroutine on a running kernel runtime.
- Route callbacks: `galay_http_server_add_route` registers exact method/path handlers. `serve_one` accepts one connection, reads one request, invokes the handler, sends one response, and closes the session.
- Streaming smoke surface: `galay_http_session_send_bytes` and `galay_http_session_recv_bytes` expose raw session bytes for split-response, timeout, malformed, and closed-peer tests.

## Request And Response Helpers

Use `galay_http_request_create` / `galay_http_response_create` before filling or
parsing objects. Request setters cover method/path, headers, and body:
`galay_http_request_set_method_path`, `galay_http_request_add_header`, and
`galay_http_request_set_body`. Response setters cover status, headers, and body:
`galay_http_response_set_status`, `galay_http_response_add_header`, and
`galay_http_response_set_body`.

`galay_http_request_parse` and `galay_http_response_parse` consume HTTP/1.1 bytes
into an existing object. `max_header_len` and `max_body_len` bound accepted input,
and `consumed` reports how many bytes were parsed. `galay_http_request_is_complete`
indicates whether a parsed request has enough data for the current message.

`galay_http_request_serialize` and `galay_http_response_serialize` return borrowed
buffers owned by the request/response object. The pointer remains valid until the
object is mutated, parsed/serialized again, or destroyed. Accessors such as
`galay_http_request_method`, `galay_http_request_path`,
`galay_http_request_body`, `galay_http_request_find_header`,
`galay_http_response_status`, and `galay_http_response_body` also return borrowed
data tied to the owning object lifetime.

## Async Client, Server, And Session

Client flow: create with `galay_http_client_create`, connect with
`galay_http_client_connect`, send with `galay_http_client_send_request`, receive
an owned response from `galay_http_client_recv_response`, close with
`galay_http_client_close`, then destroy with `galay_http_client_destroy`.

Server flow: create with `galay_http_server_create`, bind/listen with
`galay_http_server_bind` and `galay_http_server_listen`, inspect the bound address
with `galay_http_server_local_endpoint`, register callbacks with
`galay_http_server_add_route`, and accept work through `galay_http_server_accept`
or `galay_http_server_serve_one`. `galay_http_server_stop` closes the listener
path before `galay_http_server_destroy`.

`galay_http_server_accept` returns an owned `galay_http_session_t*`; release it
with `galay_http_session_destroy` after `galay_http_session_close` or after the
peer is otherwise closed. Session helpers send/receive full HTTP messages through
`galay_http_session_send_request`, `galay_http_session_recv_request`,
`galay_http_session_send_response`, and `galay_http_session_recv_response`.
`galay_http_session_send_bytes` and `galay_http_session_recv_bytes` are raw byte
helpers for streaming/protocol tests; caller buffers are not retained.

Route callback `request` and `response` pointers are borrowed and valid only for
the callback invocation. The callback fills `response` and returns
`galay_status_t`; non-`GALAY_OK` is converted into the C HTTP error path.

## Error Model

Synchronous create/bind/listen/register functions return `galay_status_t`; use `galay_http_get_error` for readable text. Suspending I/O returns `C_IOResult` from the kernel C coroutine ABI:

- `C_IOResultOk`: operation completed.
- `C_IOResultTimeout`: timeout expired before completion.
- `C_IOResultEof`: peer closed or zero-byte transfer.
- `C_IOResultInvalid`: invalid handle, invalid state, or call outside a C coroutine.
- `C_IOResultError`: protocol, allocation, or kernel I/O failure.

All timeout-bearing APIs use `timeout_ms`: negative waits indefinitely, zero is a
nonblocking timeout attempt, and positive values are millisecond deadlines. Close
or stop APIs are explicit and return `C_IOResult`; peer shutdown is surfaced as
`C_IOResultEof`, while invalid handles, invalid states, or calls outside a C
coroutine are surfaced as `C_IOResultInvalid`.

The implementation does not add C++ `try`/`catch`/`throw` paths.

## Deferred Areas

This C layer intentionally does not claim full C++ HTTP parity yet. TLS/HTTPS, keep-alive connection loops, chunked transfer decoding, static file/proxy helpers, router patterns, and HTTP/2 remain C++-only or separate module work.
