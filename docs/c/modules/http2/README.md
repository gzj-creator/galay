# HTTP/2 C API

`galay/c/galay-http2-c/http2.h` exposes a C ABI for HTTP/2 helper and h2c runtime use.

## Scope

- Frame, SETTINGS, PING, stream-id, and lightweight HPACK helper APIs remain synchronous helpers:
  `galay_http2_stream_id_validate`, `galay_http2_settings_value_validate`,
  `galay_http2_ping_frame_create`, `galay_http2_frame_encode`,
  `galay_http2_frame_decode`, `galay_http2_headers_add`,
  `galay_http2_headers_get`, `galay_http2_hpack_encode`, and
  `galay_http2_hpack_decode`.
- h2c prior-knowledge client/server runtime is available through opaque handles:
  `galay_http2_client_t`, `galay_http2_server_t`, `galay_http2_conn_t`, and
  `galay_http2_stream_t`.
- TLS `h2` client/server examples are deferred until the SSL C async socket integration is available.

## Coroutine Contract

The following APIs may suspend and must be called inside a `galay_coro_spawn` C coroutine:

- `galay_http2_client_connect`
- `galay_http2_client_open_stream`
- `galay_http2_server_accept`
- `galay_http2_conn_accept_stream`
- `galay_http2_conn_read_control`
- `galay_http2_conn_send_goaway`
- `galay_http2_conn_send_window_update`
- `galay_http2_stream_read_headers`
- `galay_http2_stream_write_headers`
- `galay_http2_stream_read_data`
- `galay_http2_stream_write_data`
- `galay_http2_stream_reset`

They return `C_IOResult`; protocol detail is carried in `result.value` as
`galay_http2_error_code_t`.

## Ownership

- `galay_http2_client_create` / `galay_http2_client_destroy` own the client and its client-side connection.
- `galay_http2_server_create` / `galay_http2_server_destroy` own the listening socket.
- `galay_http2_server_accept` returns an owning server-side `galay_http2_conn_t*`; release it with
  `galay_http2_conn_destroy`.
- `galay_http2_client_open_stream` and `galay_http2_conn_accept_stream` return owning stream handles;
  release them with `galay_http2_stream_destroy`.
- `galay_http2_stream_read_headers` returns a new `galay_http2_headers_t*`; release it with
  `galay_http2_headers_destroy`.
- `galay_http2_stream_read_data` copies bytes into the caller buffer and does not retain it.

## Flow Control And Lifecycle

Each connection and stream tracks send and receive windows. `galay_http2_stream_write_data` returns
`C_IOResultError` with `GALAY_HTTP2_ERROR_FLOW_CONTROL` when the payload exceeds the peer-advertised
window. A peer `WINDOW_UPDATE` is processed by `galay_http2_conn_read_control`.

`RST_STREAM` closes the stream and read APIs report `GALAY_HTTP2_ERROR_STREAM_RESET`.
Writes on closed or locally reset streams report `GALAY_HTTP2_ERROR_STREAM_CLOSED`.
After `GOAWAY`, new client streams fail with `GALAY_HTTP2_ERROR_GOAWAY`.

## Examples And Benchmarks

- `examples/c/http2/e1_h2c_client.c`
- `examples/c/http2/e2_h2c_server.c`
- `benchmark/c/http2/b1_multiplex_throughput.c`
- `benchmark/c/http2/b2_stream_pressure.c`

All examples and benchmarks use local loopback and require no external HTTP/2 service.
