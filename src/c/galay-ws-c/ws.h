#ifndef GALAY_C_WS_WS_H
#define GALAY_C_WS_WS_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_ws_opcode_t {
    GALAY_WS_OPCODE_CONTINUATION = 0x0,
    GALAY_WS_OPCODE_TEXT = 0x1,
    GALAY_WS_OPCODE_BINARY = 0x2,
    GALAY_WS_OPCODE_CLOSE = 0x8,
    GALAY_WS_OPCODE_PING = 0x9,
    GALAY_WS_OPCODE_PONG = 0xA
} galay_ws_opcode_t;

typedef enum galay_ws_close_code_t {
    GALAY_WS_CLOSE_NORMAL = 1000,
    GALAY_WS_CLOSE_PROTOCOL_ERROR = 1002
} galay_ws_close_code_t;

typedef enum galay_ws_error_t {
    GALAY_WS_ERROR_NONE = 0,
    GALAY_WS_ERROR_INCOMPLETE = 1,
    GALAY_WS_ERROR_INVALID_OPCODE = 2,
    GALAY_WS_ERROR_MASK_REQUIRED = 3,
    GALAY_WS_ERROR_MASK_UNEXPECTED = 4,
    GALAY_WS_ERROR_CONTROL_TOO_LARGE = 5,
    GALAY_WS_ERROR_CONTROL_FRAGMENTED = 6,
    GALAY_WS_ERROR_RESERVED_BITS = 7,
    GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH = 8,
    GALAY_WS_ERROR_INVALID_FRAGMENT = 9,
    GALAY_WS_ERROR_UPGRADE_FAILED = 10
} galay_ws_error_t;

typedef struct galay_ws_client_t galay_ws_client_t;
typedef struct galay_ws_session_t galay_ws_session_t;
typedef struct galay_ws_connection_t galay_ws_connection_t;
typedef struct galay_ws_received_frame_t galay_ws_received_frame_t;

/**
 * @brief WebSocket C client 配置。
 * @details host/path 在 create 时复制；port 为 0、host 为空或 path 为空会返回参数错误。
 */
typedef struct galay_ws_client_config_t {
    const char* host;          ///< IPv4/IPv6 地址或主机名；当前最小实现面向 loopback/IP。
    uint16_t port;            ///< 目标 TCP 端口。
    const char* path;         ///< HTTP upgrade path，例如 "/chat"。
    int connect_timeout_ms;   ///< connect timeout；小于 0 时由调用 connect 的 timeout 决定。
} galay_ws_client_config_t;

typedef struct galay_ws_frame_t {
    galay_bool_t fin;
    galay_ws_opcode_t opcode;
    galay_bool_t masked;
    uint64_t payload_len;
    uint8_t masking_key[4];
} galay_ws_frame_t;

const char* galay_ws_get_error(galay_ws_error_t error);
galay_status_t galay_ws_encoded_size(size_t payload_len, galay_bool_t masked,
                                     size_t* encoded_size);
galay_status_t galay_ws_apply_mask(uint8_t* data, size_t len, const uint8_t mask_key[4]);
galay_status_t galay_ws_encode_frame(galay_ws_opcode_t opcode, const uint8_t* payload,
                                     size_t payload_len, galay_bool_t fin,
                                     const uint8_t mask_key[4], uint8_t* out,
                                     size_t out_len, size_t* written);
galay_status_t galay_ws_decode_frame(const uint8_t* data, size_t data_len,
                                     galay_bool_t expect_masked,
                                     galay_ws_frame_t* frame, uint8_t* payload_out,
                                     size_t payload_out_len, size_t* consumed,
                                     galay_ws_error_t* ws_error);

/**
 * @brief 创建 WebSocket client handle。
 * @param config client 配置，host/path 会被复制。
 * @param out 成功时返回 client；调用方用 `galay_ws_client_destroy` 释放。
 * @return 参数错误或内存不足通过 `galay_status_t` 返回。
 */
galay_status_t galay_ws_client_create(const galay_ws_client_config_t* config,
                                      galay_ws_client_t** out);

/**
 * @brief 销毁 WebSocket client 及其拥有的 connection。
 */
void galay_ws_client_destroy(galay_ws_client_t* client);

/**
 * @brief 在当前 C coroutine 内连接 TCP endpoint 并完成 client upgrade。
 * @param client 由 `galay_ws_client_create` 创建的 client。
 * @param timeout_ms 每次 socket I/O timeout；负数表示无限等待。
 * @param out_connection 成功时返回借用 connection，生命周期到 client destroy。
 * @return `C_IOResultOk` 表示升级成功；错误通过 `C_IOResult` 显式传播。
 */
C_IOResult galay_ws_client_connect(galay_ws_client_t* client,
                                   int64_t timeout_ms,
                                   galay_ws_connection_t** out_connection);

/**
 * @brief 从已 accept 的 TCP socket 创建 server/client session。
 * @details 成功后 socket 所有权转移给 session，输入 socket 会被置空。
 */
galay_status_t galay_ws_session_adopt_tcp(galay_kernel_tcp_socket_t* socket,
                                          galay_bool_t server_side,
                                          galay_ws_session_t** out);

/**
 * @brief 销毁 session 及其拥有的 connection。
 */
void galay_ws_session_destroy(galay_ws_session_t* session);

/**
 * @brief 在当前 C coroutine 内读取 HTTP upgrade 请求并返回 101 响应。
 */
C_IOResult galay_ws_session_accept_upgrade(galay_ws_session_t* session, int64_t timeout_ms);

/**
 * @brief 获取 session 拥有的 connection 借用指针。
 */
galay_status_t galay_ws_session_connection(galay_ws_session_t* session,
                                           galay_ws_connection_t** out_connection);

/**
 * @brief 发送单个 WebSocket frame。
 * @details client-side connection 会自动按 RFC 要求添加 mask；server-side 不添加 mask。
 */
C_IOResult galay_ws_connection_send_frame(galay_ws_connection_t* connection,
                                          galay_ws_opcode_t opcode,
                                          const uint8_t* payload,
                                          size_t payload_len,
                                          galay_bool_t fin,
                                          int64_t timeout_ms);

C_IOResult galay_ws_connection_send_text(galay_ws_connection_t* connection,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         int64_t timeout_ms);
C_IOResult galay_ws_connection_send_binary(galay_ws_connection_t* connection,
                                           const uint8_t* payload,
                                           size_t payload_len,
                                           int64_t timeout_ms);
C_IOResult galay_ws_connection_send_ping(galay_ws_connection_t* connection,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         int64_t timeout_ms);
C_IOResult galay_ws_connection_send_pong(galay_ws_connection_t* connection,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         int64_t timeout_ms);
C_IOResult galay_ws_connection_send_close(galay_ws_connection_t* connection,
                                          galay_ws_close_code_t close_code,
                                          const uint8_t* reason,
                                          size_t reason_len,
                                          int64_t timeout_ms);

/**
 * @brief 接收单个 WebSocket frame。
 * @param out_frame 成功时返回 frame，调用方用 `galay_ws_received_frame_destroy` 释放。
 * @return 协议错误时 `C_IOResult.value` 保存 `galay_ws_error_t`。
 */
C_IOResult galay_ws_connection_recv_frame(galay_ws_connection_t* connection,
                                          int64_t timeout_ms,
                                          galay_ws_received_frame_t** out_frame);

/**
 * @brief 关闭底层 TCP connection；不销毁 handle。
 */
C_IOResult galay_ws_connection_close(galay_ws_connection_t* connection, int64_t timeout_ms);

galay_ws_opcode_t galay_ws_received_frame_opcode(const galay_ws_received_frame_t* frame);
galay_bool_t galay_ws_received_frame_fin(const galay_ws_received_frame_t* frame);
galay_bool_t galay_ws_received_frame_masked(const galay_ws_received_frame_t* frame);
galay_status_t galay_ws_received_frame_payload(const galay_ws_received_frame_t* frame,
                                               const uint8_t** payload,
                                               size_t* payload_len);
void galay_ws_received_frame_destroy(galay_ws_received_frame_t* frame);

#ifdef __cplusplus
}
#endif

#endif
