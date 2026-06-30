#ifndef GALAY_C_WS_WS_H
#define GALAY_C_WS_WS_H

/**
 * @file ws.h
 * @brief Galay WebSocket C ABI。
 *
 * @details 该头文件暴露最小 WebSocket C runtime surface：frame codec helper、
 * client upgrade、server session upgrade、connection frame I/O 以及 received-frame
 * 访问器。网络 I/O API 基于 kernel direct C coroutine TCP API，并以 `C_IOResult`
 * 显式返回 timeout、cancel、EOF、协议错误和底层 I/O 错误。
 */

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WebSocket frame opcode。
 * @note control opcode 会受到 FIN 和 125 字节 payload 上限校验。
 */
typedef enum galay_ws_opcode_t {
    GALAY_WS_OPCODE_CONTINUATION = 0x0,   ///< continuation frame。
    GALAY_WS_OPCODE_TEXT = 0x1,           ///< text data frame。
    GALAY_WS_OPCODE_BINARY = 0x2,         ///< binary data frame。
    GALAY_WS_OPCODE_CLOSE = 0x8,          ///< close control frame。
    GALAY_WS_OPCODE_PING = 0x9,           ///< ping control frame。
    GALAY_WS_OPCODE_PONG = 0xA            ///< pong control frame。
} galay_ws_opcode_t;

/**
 * @brief WebSocket close code。
 */
typedef enum galay_ws_close_code_t {
    GALAY_WS_CLOSE_NORMAL = 1000,             ///< 正常关闭。
    GALAY_WS_CLOSE_PROTOCOL_ERROR = 1002      ///< 协议错误关闭。
} galay_ws_close_code_t;

/**
 * @brief WebSocket C ABI 协议错误码。
 * @details frame codec 和 connection recv 的协议失败会通过该枚举表达；网络 I/O 仍通过
 * `C_IOResultCode` 表达。
 */
typedef enum galay_ws_error_t {
    GALAY_WS_ERROR_NONE = 0,                       ///< 无 WebSocket 协议错误。
    GALAY_WS_ERROR_INCOMPLETE = 1,                 ///< 输入字节不足以解析完整 frame。
    GALAY_WS_ERROR_INVALID_OPCODE = 2,             ///< opcode 不在公开枚举范围内。
    GALAY_WS_ERROR_MASK_REQUIRED = 3,              ///< server side 收到未 mask client frame。
    GALAY_WS_ERROR_MASK_UNEXPECTED = 4,            ///< client side 收到 masked server frame。
    GALAY_WS_ERROR_CONTROL_TOO_LARGE = 5,          ///< control frame payload 超过 125 字节。
    GALAY_WS_ERROR_CONTROL_FRAGMENTED = 6,         ///< control frame 被分片。
    GALAY_WS_ERROR_RESERVED_BITS = 7,              ///< RSV 位被设置。
    GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH = 8,     ///< payload length 编码或 close payload 无效。
    GALAY_WS_ERROR_INVALID_FRAGMENT = 9,           ///< continuation/data frame 顺序无效。
    GALAY_WS_ERROR_UPGRADE_FAILED = 10             ///< HTTP WebSocket upgrade 校验失败。
} galay_ws_error_t;

/**
 * @brief WebSocket client opaque handle。
 * @details client 保存 host/port/path 配置并拥有成功 connect 后的 connection。
 * @note handle 不提供内部同步；同一 client 应在同一线程/协程上下文中串行访问。
 */
typedef struct galay_ws_client_t galay_ws_client_t;

/**
 * @brief WebSocket server-side session opaque handle。
 * @details session 接管一个已 accept 的 TCP socket，完成 server upgrade 后拥有 connection。
 * @note session destroy 会释放其 connection；从 session 取出的 connection 是借用指针。
 */
typedef struct galay_ws_session_t galay_ws_session_t;

/**
 * @brief WebSocket connection opaque handle。
 * @details connection 拥有底层 TCP socket、收包缓冲、mask 计数和分片状态。
 * @note send/recv/close API 必须在 `galay_coro_spawn` 创建的 C coroutine 内调用；
 * 同一 connection 不应被多个线程或 coroutine 并发操作。
 */
typedef struct galay_ws_connection_t galay_ws_connection_t;

/**
 * @brief 接收到的 WebSocket frame opaque handle。
 * @details 由 `galay_ws_connection_recv_frame` 创建，保存 frame metadata 和自有 payload。
 * @note payload 访问器返回借用指针，仅在 frame destroy 前有效。
 */
typedef struct galay_ws_received_frame_t galay_ws_received_frame_t;

/**
 * @brief WebSocket C client 配置。
 * @details host/path 在 `galay_ws_client_create` 时复制；port 为 0、host 为空或 path
 * 为空会返回参数错误。
 * @note `connect_timeout_ms` 小于 0 时由 `galay_ws_client_connect` 的 timeout 参数决定。
 */
typedef struct galay_ws_client_config_t {
    const char* host;          ///< IPv4/IPv6 地址或主机名；当前最小实现面向 loopback/IP。
    uint16_t port;            ///< 目标 TCP 端口；0 无效。
    const char* path;         ///< HTTP upgrade path，例如 "/chat"；不能为空。
    int connect_timeout_ms;   ///< connect timeout；小于 0 时使用 connect 调用参数。
} galay_ws_client_config_t;

/**
 * @brief 解码后的 WebSocket frame metadata。
 * @details 该结构不拥有 payload；payload 字节由 decode 调用方的输出缓冲区或
 * `galay_ws_received_frame_t` 保存。
 */
typedef struct galay_ws_frame_t {
    galay_bool_t fin;              ///< 是否为消息最后一个 frame。
    galay_ws_opcode_t opcode;      ///< frame opcode。
    galay_bool_t masked;           ///< frame 是否携带 mask。
    uint64_t payload_len;          ///< 解码后的 payload 字节数。
    uint8_t masking_key[4];        ///< frame mask key；未 masked 时为 0。
} galay_ws_frame_t;

/**
 * @brief 将 WebSocket 协议错误码转换为可读字符串。
 * @param error WebSocket 协议错误码。
 * @return 静态错误字符串，调用方不得释放或修改。
 */
const char* galay_ws_get_error(galay_ws_error_t error);

/**
 * @brief 计算一个 frame 编码后的总字节数。
 * @param payload_len payload 字节数。
 * @param masked 是否包含 4 字节 mask key。
 * @param encoded_size 成功时写入 header + mask + payload 总字节数。
 * @return `GALAY_OK` 表示成功；空输出指针返回 `GALAY_INVALID_ARGUMENT`；
 *         size_t 溢出返回 `GALAY_OUT_OF_MEMORY`。
 * @note 该 helper 是同步纯计算，不挂起、不拥有任何输入。
 */
galay_status_t galay_ws_encoded_size(size_t payload_len, galay_bool_t masked,
                                     size_t* encoded_size);

/**
 * @brief 原地应用 WebSocket mask。
 * @param data 待异或的数据；`len` 为 0 时可为 NULL。
 * @param len 数据字节数。
 * @param mask_key 4 字节 mask key。
 * @return `GALAY_OK` 表示成功；无效指针返回 `GALAY_INVALID_ARGUMENT`。
 * @note 该函数会修改 data 内容；调用两次同一 mask 可恢复原始 payload。
 */
galay_status_t galay_ws_apply_mask(uint8_t* data, size_t len, const uint8_t mask_key[4]);

/**
 * @brief 编码单个 WebSocket frame。
 * @param opcode frame opcode，必须是公开枚举中的有效值。
 * @param payload payload 字节；`payload_len` 为 0 时可为 NULL。
 * @param payload_len payload 字节数。
 * @param fin 是否设置 FIN。
 * @param mask_key 为 NULL 时编码 unmasked frame；非 NULL 时使用该 4 字节 key mask payload。
 * @param out 调用方提供的输出缓冲区。
 * @param out_len 输出缓冲区字节数。
 * @param written 成功时写入实际编码字节数；失败时写入 0。
 * @return `GALAY_OK` 表示成功；无效 opcode/参数返回 `GALAY_INVALID_ARGUMENT`；
 *         输出缓冲区不足或大小溢出返回 `GALAY_OUT_OF_MEMORY`。
 * @note payload 和 mask_key 仅在调用期间借用；out 由调用方拥有。
 */
galay_status_t galay_ws_encode_frame(galay_ws_opcode_t opcode, const uint8_t* payload,
                                     size_t payload_len, galay_bool_t fin,
                                     const uint8_t mask_key[4], uint8_t* out,
                                     size_t out_len, size_t* written);

/**
 * @brief 解码单个 WebSocket frame。
 * @param data 输入 frame 字节。
 * @param data_len 输入字节数。
 * @param expect_masked `GALAY_TRUE` 要求输入 masked，`GALAY_FALSE` 要求输入 unmasked。
 * @param frame 成功时写入 frame metadata。
 * @param payload_out 调用方提供的 payload 输出缓冲区；payload 为 0 字节时可为 NULL。
 * @param payload_out_len payload_out 字节数。
 * @param consumed 成功时写入消费字节数；失败时写入 0。
 * @param ws_error 成功时写入 `GALAY_WS_ERROR_NONE`；协议失败时写入具体原因。
 * @return `GALAY_OK` 表示完整 frame 解码成功；参数错误返回 `GALAY_INVALID_ARGUMENT`；
 *         不完整或协议错误返回 `GALAY_PROTOCOL_ERROR` 并设置 `ws_error`。
 * @note `GALAY_WS_ERROR_INCOMPLETE` 也通过 `GALAY_PROTOCOL_ERROR` 返回，调用方可继续累积输入。
 */
galay_status_t galay_ws_decode_frame(const uint8_t* data, size_t data_len,
                                     galay_bool_t expect_masked,
                                     galay_ws_frame_t* frame, uint8_t* payload_out,
                                     size_t payload_out_len, size_t* consumed,
                                     galay_ws_error_t* ws_error);

/**
 * @brief 创建 WebSocket client handle。
 * @param config client 配置，host/path 会被复制。
 * @param out 成功时返回 client；失败时写入 NULL。
 * @return `GALAY_OK` 表示成功；参数错误或内存不足通过 `galay_status_t` 返回。
 * @note 创建本身不连接网络、不阻塞、不挂起。
 */
galay_status_t galay_ws_client_create(const galay_ws_client_config_t* config,
                                      galay_ws_client_t** out);

/**
 * @brief 销毁 WebSocket client 及其拥有的 connection。
 * @param client 可为 NULL；销毁后通过 client 取得的 connection 借用指针全部失效。
 * @note 需要协议层 close frame 或 TCP close 时先调用 send_close/connection_close，再 destroy。
 */
void galay_ws_client_destroy(galay_ws_client_t* client);

/**
 * @brief 在当前 C coroutine 内连接 TCP endpoint 并完成 client upgrade。
 * @param client 由 `galay_ws_client_create` 创建且尚未 connect 的 client。
 * @param timeout_ms 每次 socket I/O timeout；负数表示无限等待。若该值为负且 config
 *        `connect_timeout_ms` 为正，则 connect 和 upgrade 使用 config timeout。
 * @param out_connection 成功时返回借用 connection，生命周期到 client destroy。
 * @return `C_IOResultOk` 表示 TCP connect 和 HTTP upgrade 成功，`ptr` 指向同一 connection；
 *         参数错误、timeout、cancel、EOF、upgrade 失败或底层 I/O 错误通过 `C_IOResult` 返回。
 * @note 该函数会挂起当前 C coroutine，不阻塞 OS 线程；同一 client 只能成功 connect 一次。
 */
C_IOResult galay_ws_client_connect(galay_ws_client_t* client,
                                   int64_t timeout_ms,
                                   galay_ws_connection_t** out_connection);

/**
 * @brief 从已 accept 的 TCP socket 创建 server/client session。
 * @details 成功后 socket 所有权转移给 session，输入 socket 会被置空。
 * @param socket 已 accept 的 kernel TCP socket；成功后不再由调用方 destroy。
 * @param server_side `GALAY_TRUE` 表示按 server 侧 mask 规则接收/发送，`GALAY_FALSE`
 *        表示按 client 侧规则。
 * @param out 成功时写入新建 session；失败时写入 NULL。
 * @return `GALAY_OK` 表示接管成功；参数错误或内存不足通过 `galay_status_t` 返回。
 * @note 该函数只转移 ownership，不执行 HTTP upgrade，不挂起。
 */
galay_status_t galay_ws_session_adopt_tcp(galay_kernel_tcp_socket_t* socket,
                                          galay_bool_t server_side,
                                          galay_ws_session_t** out);

/**
 * @brief 销毁 session 及其拥有的 connection。
 * @param session 可为 NULL；销毁后从 session 取得的 connection 借用指针全部失效。
 * @note destroy 不发送 close frame；需要协议关闭时先调用 send_close/connection_close。
 */
void galay_ws_session_destroy(galay_ws_session_t* session);

/**
 * @brief 在当前 C coroutine 内读取 HTTP upgrade 请求并返回 101 响应。
 * @param session server-side session，必须来自 `galay_ws_session_adopt_tcp(..., GALAY_TRUE, ...)`。
 * @param timeout_ms 每次 socket I/O timeout；负数无限等待，0 立即超时，正数为毫秒超时。
 * @return `C_IOResultOk` 表示 upgrade 成功，`ptr` 指向 session；参数错误、timeout、
 *         cancel、EOF、upgrade 失败或底层 I/O 错误通过 `C_IOResult` 返回。
 * @note 该函数会挂起当前 C coroutine；同一 session 只能成功 upgrade 一次。
 */
C_IOResult galay_ws_session_accept_upgrade(galay_ws_session_t* session, int64_t timeout_ms);

/**
 * @brief 获取 session 拥有的 connection 借用指针。
 * @param session session handle。
 * @param out_connection 成功时写入 borrowed connection；失败时写入 NULL。
 * @return `GALAY_OK` 表示成功；空 handle 或无 connection 返回 `GALAY_INVALID_ARGUMENT`。
 * @note 返回指针由 session 拥有，在 session destroy 后失效。
 */
galay_status_t galay_ws_session_connection(galay_ws_session_t* session,
                                           galay_ws_connection_t** out_connection);

/**
 * @brief 发送单个 WebSocket frame。
 * @details client-side connection 会自动按 RFC 要求添加 mask；server-side 不添加 mask。
 * @param connection 已 upgrade 的 connection。
 * @param opcode frame opcode。
 * @param payload payload 字节；`payload_len` 为 0 时可为 NULL。
 * @param payload_len payload 字节数。
 * @param fin 是否设置 FIN。
 * @param timeout_ms socket send timeout；负数无限等待，0 立即超时，正数为毫秒超时。
 * @return `C_IOResultOk` 表示 frame 已完整发送，`bytes` 为编码后发送字节数，
 *         `value` 保存 opcode；参数错误、timeout、cancel、EOF 或 I/O 错误通过
 *         `C_IOResult` 返回。
 * @note payload 在函数返回前必须保持有效；函数不会在返回后保存 payload 指针。
 */
C_IOResult galay_ws_connection_send_frame(galay_ws_connection_t* connection,
                                          galay_ws_opcode_t opcode,
                                          const uint8_t* payload,
                                          size_t payload_len,
                                          galay_bool_t fin,
                                          int64_t timeout_ms);

/**
 * @brief 发送完整 text frame。
 * @param connection 已 upgrade 的 connection。
 * @param payload UTF-8 text payload 字节；当前 C ABI 不做 UTF-8 校验。
 * @param payload_len payload 字节数。
 * @param timeout_ms socket send timeout。
 * @return 参见 `galay_ws_connection_send_frame`；成功时 `value` 为 `GALAY_WS_OPCODE_TEXT`。
 */
C_IOResult galay_ws_connection_send_text(galay_ws_connection_t* connection,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         int64_t timeout_ms);

/**
 * @brief 发送完整 binary frame。
 * @param connection 已 upgrade 的 connection。
 * @param payload binary payload 字节；`payload_len` 为 0 时可为 NULL。
 * @param payload_len payload 字节数。
 * @param timeout_ms socket send timeout。
 * @return 参见 `galay_ws_connection_send_frame`；成功时 `value` 为 `GALAY_WS_OPCODE_BINARY`。
 */
C_IOResult galay_ws_connection_send_binary(galay_ws_connection_t* connection,
                                           const uint8_t* payload,
                                           size_t payload_len,
                                           int64_t timeout_ms);

/**
 * @brief 发送 ping control frame。
 * @param connection 已 upgrade 的 connection。
 * @param payload ping payload；`payload_len` 为 0 时可为 NULL。
 * @param payload_len payload 字节数，必须不超过 125。
 * @param timeout_ms socket send timeout。
 * @return 成功时 `value` 为 `GALAY_WS_OPCODE_PING`；payload 过大时 `C_IOResult.value`
 *         保存 `GALAY_WS_ERROR_CONTROL_TOO_LARGE`。
 */
C_IOResult galay_ws_connection_send_ping(galay_ws_connection_t* connection,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         int64_t timeout_ms);

/**
 * @brief 发送 pong control frame。
 * @param connection 已 upgrade 的 connection。
 * @param payload pong payload；`payload_len` 为 0 时可为 NULL。
 * @param payload_len payload 字节数，必须不超过 125。
 * @param timeout_ms socket send timeout。
 * @return 成功时 `value` 为 `GALAY_WS_OPCODE_PONG`；payload 过大时 `C_IOResult.value`
 *         保存 `GALAY_WS_ERROR_CONTROL_TOO_LARGE`。
 */
C_IOResult galay_ws_connection_send_pong(galay_ws_connection_t* connection,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         int64_t timeout_ms);

/**
 * @brief 发送 close control frame。
 * @param connection 已 upgrade 的 connection。
 * @param close_code close code。
 * @param reason UTF-8 close reason 字节；`reason_len` 为 0 时可为 NULL。
 * @param reason_len reason 字节数，必须不超过 123。
 * @param timeout_ms socket send timeout。
 * @return 成功时 `value` 为 `GALAY_WS_OPCODE_CLOSE`；无效 reason 参数返回
 *         `C_IOResultInvalid`，其他错误通过 `C_IOResult` 返回。
 * @note 发送 close frame 不会销毁 connection；需要关闭 TCP 时继续调用
 * `galay_ws_connection_close`。
 */
C_IOResult galay_ws_connection_send_close(galay_ws_connection_t* connection,
                                          galay_ws_close_code_t close_code,
                                          const uint8_t* reason,
                                          size_t reason_len,
                                          int64_t timeout_ms);

/**
 * @brief 接收单个 WebSocket frame。
 * @param connection 已 upgrade 的 connection。
 * @param timeout_ms socket recv timeout；负数无限等待，0 立即超时，正数为毫秒超时。
 * @param out_frame 成功时返回新分配 frame；失败时写入 NULL。
 * @return `C_IOResultOk` 表示成功，`ptr` 指向同一个 frame，`bytes` 为消费字节数，
 *         `value` 保存 opcode；协议错误时 `C_IOResult.value` 保存 `galay_ws_error_t`；
 *         EOF、timeout、cancel 或 I/O 错误通过 `C_IOResult.code` 返回。
 * @note 调用方必须用 `galay_ws_received_frame_destroy` 释放成功返回的 frame。
 */
C_IOResult galay_ws_connection_recv_frame(galay_ws_connection_t* connection,
                                          int64_t timeout_ms,
                                          galay_ws_received_frame_t** out_frame);

/**
 * @brief 关闭底层 TCP connection；不销毁 handle。
 * @param connection connection handle。
 * @param timeout_ms close 操作 timeout。
 * @return `C_IOResultOk` 表示 TCP close 成功；空 handle、timeout、cancel 或 I/O 错误通过
 *         `C_IOResult` 返回。
 * @note close 后仍需由 owner 调用 `galay_ws_client_destroy` 或 `galay_ws_session_destroy`。
 */
C_IOResult galay_ws_connection_close(galay_ws_connection_t* connection, int64_t timeout_ms);

/**
 * @brief 读取 received frame opcode。
 * @param frame received frame；可为 NULL。
 * @return frame opcode；frame 为 NULL 时返回 `GALAY_WS_OPCODE_CLOSE` 作为哨兵值。
 */
galay_ws_opcode_t galay_ws_received_frame_opcode(const galay_ws_received_frame_t* frame);

/**
 * @brief 读取 received frame FIN 标志。
 * @param frame received frame；可为 NULL。
 * @return frame 非空时返回其 FIN，否则返回 `GALAY_FALSE`。
 */
galay_bool_t galay_ws_received_frame_fin(const galay_ws_received_frame_t* frame);

/**
 * @brief 读取 received frame masked 标志。
 * @param frame received frame；可为 NULL。
 * @return frame 非空时返回其 masked 状态，否则返回 `GALAY_FALSE`。
 */
galay_bool_t galay_ws_received_frame_masked(const galay_ws_received_frame_t* frame);

/**
 * @brief 读取 received frame payload。
 * @param frame received frame。
 * @param payload 成功时写入借用 payload 指针；空 payload 时写入 NULL。
 * @param payload_len 成功时写入 payload 字节数。
 * @return `GALAY_OK` 表示成功；空指针返回 `GALAY_INVALID_ARGUMENT`。
 * @note payload 由 frame 拥有，仅在 frame destroy 前有效；调用方不得释放或修改。
 */
galay_status_t galay_ws_received_frame_payload(const galay_ws_received_frame_t* frame,
                                               const uint8_t** payload,
                                               size_t* payload_len);

/**
 * @brief 销毁 received frame。
 * @param frame 可为 NULL；销毁后其 payload 借用指针失效。
 */
void galay_ws_received_frame_destroy(galay_ws_received_frame_t* frame);

#ifdef __cplusplus
}
#endif

#endif
