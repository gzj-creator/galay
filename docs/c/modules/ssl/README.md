# SSL C API

`galay-ssl-c` exposes OpenSSL-backed TLS context and socket handles for the direct C coroutine runtime.

## Handles And Ownership

- `galay_ssl_context_t` owns a C++ `galay::ssl::SslContext`. Create it with `galay_ssl_context_create` and release it with `galay_ssl_context_destroy`.
- `galay_ssl_socket_t` owns a kernel TCP socket plus a C++ `galay::ssl::SslEngine`. Create it with `galay_ssl_socket_create`; accepted sockets are returned by `galay_ssl_socket_accept`. Release every socket with `galay_ssl_socket_destroy`.
- A socket borrows its context. Keep the context alive until all sockets created from it are destroyed.

## Coroutine I/O

`galay_ssl_socket_accept`, `connect`, `handshake`, `recv`, `send`, `shutdown`, and `close` return `C_IOResult` and must be called from a C coroutine spawned with `galay_coro_spawn`. These functions drive TLS through the existing C TCP coroutine ABI and do not expose C++ coroutine types.

`recv` writes plaintext into caller-owned memory and reports bytes read in `C_IOResult.bytes`. `send` borrows the caller buffer for the duration of the call and reports plaintext bytes accepted in `C_IOResult.bytes`.

## Errors

Configuration APIs return `galay_status_t`; socket coroutine APIs return `C_IOResult`. Invalid handles or invalid buffers return `GALAY_INVALID_ARGUMENT` or `C_IOResultInvalid`. TLS protocol, certificate, or transport failures are mapped to `GALAY_IO_ERROR`, `C_IOResultError`, or `C_IOResultEof`.

## ALPN

Configure ALPN on the context before creating sockets from that context:

- `galay_ssl_context_set_alpn_protocols` sets the client offer list.
- `galay_ssl_context_set_alpn_select_protocols` sets the server selection priority. If client and server overlap, the first server-preferred protocol wins.

The protocol array and strings are borrowed only for the duration of the call. Each protocol must be non-empty and no longer than 255 bytes. Invalid lists return `GALAY_INVALID_ARGUMENT`; OpenSSL configuration failures return `GALAY_IO_ERROR`.

After a successful handshake, `galay_ssl_socket_get_negotiated_alpn` copies the negotiated protocol into caller-owned memory and writes the byte count to `written`. The copied bytes are not NUL-terminated. If no ALPN protocol was negotiated, `written` is `0`.

## Session Controls

Session controls are context-level settings and should be applied before sockets are created:

- `galay_ssl_context_set_session_cache_mode` accepts `GALAY_SSL_SESSION_CACHE_OFF`, `CLIENT`, `SERVER`, or `BOTH`.
- `galay_ssl_context_set_session_timeout` sets the session timeout in seconds; negative values are invalid.
- `galay_ssl_context_disable_session_cache` disables OpenSSL session caching for the context.
- `galay_ssl_context_disable_session_tickets` disables TLS session tickets for the context.

These APIs do not expose `SSL_SESSION*` or any C++ type through the C ABI. They only configure the underlying C++ `SslContext` and return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.

## Notes

- Certificate/key/CA loading uses the real C++ SSL context and validates file existence before loading so missing files return `GALAY_NOT_FOUND`.
- `galay_ssl_socket_set_hostname` configures SNI and hostname verification on client sockets before handshake.
- `galay_ssl_socket_get_protocol`, `galay_ssl_socket_get_cipher`, and `galay_ssl_socket_get_negotiated_alpn` copy negotiated strings into caller-owned buffers after handshake.
