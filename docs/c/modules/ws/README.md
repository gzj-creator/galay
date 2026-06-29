# Galay C WebSocket

## 范围

当前 C WebSocket 层提供一个真实但最小的 direct C coroutine runtime 子集：

- `galay_ws_client_t`：在 C coroutine 内 TCP connect，并完成 HTTP WebSocket upgrade。
- `galay_ws_session_t`：接管已 accept 的 `galay_kernel_tcp_socket_t`，执行 server upgrade。
- `galay_ws_connection_t`：发送/接收单个 frame，支持 text、binary、ping、pong、close。
- `galay_ws_received_frame_t`：保存接收 frame 的 opcode、FIN、mask 状态和 payload。
- codec helper：保留 `galay_ws_encode_frame`、`galay_ws_decode_frame`、mask 和 size helper。

所有会挂起的网络 API 返回 `C_IOResult`，需要在 `galay_coro_spawn` 创建的 C coroutine 内调用。
协议和参数错误通过 `C_IOResult.code`、`C_IOResult.value` 或 `galay_status_t` 显式返回，不使用 C++ 异常。

## 生命周期

client path：

1. `galay_ws_client_create(&config, &client)`
2. `galay_ws_client_connect(client, timeout_ms, &connection)`
3. `galay_ws_connection_send_text` / `galay_ws_connection_recv_frame`
4. `galay_ws_connection_send_close`，收到 peer close 后 `galay_ws_connection_close`
5. `galay_ws_client_destroy(client)`

server path：

1. 用 kernel C TCP API `bind/listen/accept`
2. `galay_ws_session_adopt_tcp(&accepted, GALAY_TRUE, &session)`，成功后 socket 所有权转移给 session
3. `galay_ws_session_accept_upgrade(session, timeout_ms)`
4. `galay_ws_session_connection(session, &connection)`
5. 发送/接收 frame，最后 `galay_ws_session_destroy(session)`

`galay_ws_received_frame_t` 由 recv API 创建，调用方必须用
`galay_ws_received_frame_destroy` 释放。payload 指针由 frame 对象拥有，frame destroy 后失效。

## 协议行为

- client 发送 frame 自动加 mask；server 发送 frame 不加 mask。
- server 接收未 mask 的 client frame 返回 `GALAY_WS_ERROR_MASK_REQUIRED`。
- client 接收 masked server frame 返回 `GALAY_WS_ERROR_MASK_UNEXPECTED`。
- control frame 会校验 FIN 和 125 字节长度限制。
- close payload 长度为 1 字节会返回 `GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH`。
- frame-level continuation 可接收；connection recv 会拒绝无起始 frame 的 continuation 或分片中插入新 data frame。

## Deferred

当前 C 层没有暴露完整 C++ WebSocket parity：

- WSS/TLS C WebSocket runtime。
- 长驻 callback server、route/subprotocol callback、自动 ping/pong policy。
- message reassembly API；现在公开的是 frame-level IO。
- permessage-deflate、UTF-8 message validation、完整 close state machine。

这些能力仍以 C++ WebSocket 模块为准，C 层先交付 loopback 可验证的最小 runtime surface。
