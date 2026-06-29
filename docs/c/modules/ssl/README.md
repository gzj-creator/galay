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

## Notes

- Certificate/key/CA loading uses the real C++ SSL context and validates file existence before loading so missing files return `GALAY_NOT_FOUND`.
- `galay_ssl_socket_set_hostname` configures SNI and hostname verification on client sockets before handshake.
- `galay_ssl_socket_get_protocol` and `galay_ssl_socket_get_cipher` copy negotiated strings into caller-owned buffers after handshake.
- ALPN and session reuse controls are not exposed yet; the C surface currently covers loopback TLS handshake, plaintext read/write, shutdown, protocol, and cipher inspection.
