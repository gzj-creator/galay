/**
 * @file rpc.h
 * @brief Galay RPC C ABI 编解码、client/server、stream、cancel 与 pool 接口。
 * @details C ABI 通过不透明 handle 管理运行期对象，通过显式 `galay_status_t`、
 * `galay_rpc_error_code_t` 和 `C_IOResult` 返回失败；不会通过 C++ 异常跨 ABI 传播。
 */
#ifndef GALAY_C_RPC_RPC_H
#define GALAY_C_RPC_RPC_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/common-c/host.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALAY_RPC_HEADER_SIZE 16u  ///< RPC frame 固定头长度，单位字节。

/**
 * @brief RPC 调用模式。
 * @details unary 和三类 streaming 共享同一帧格式；stream API 只接受 streaming 模式。
 */
typedef enum galay_rpc_call_mode_t {
    GALAY_RPC_CALL_UNARY = 1,             ///< 单请求单响应。
    GALAY_RPC_CALL_CLIENT_STREAMING = 2,  ///< 客户端流。
    GALAY_RPC_CALL_SERVER_STREAMING = 3,  ///< 服务端流。
    GALAY_RPC_CALL_BIDI_STREAMING = 4     ///< 双向流。
} galay_rpc_call_mode_t;

/**
 * @brief RPC 业务错误码。
 * @details 该错误码写入 RPC response payload 前的 error 字段，也可通过
 * `galay_rpc_error_to_status` 映射为通用 C 状态码。
 */
typedef enum galay_rpc_error_code_t {
    GALAY_RPC_ERROR_OK = 0,                    ///< 无业务错误。
    GALAY_RPC_ERROR_UNKNOWN_ERROR = 1,         ///< 未分类错误。
    GALAY_RPC_ERROR_SERVICE_NOT_FOUND = 2,     ///< service 未注册。
    GALAY_RPC_ERROR_METHOD_NOT_FOUND = 3,      ///< method 未注册或模式不匹配。
    GALAY_RPC_ERROR_INVALID_REQUEST = 4,       ///< request frame 或字段无效。
    GALAY_RPC_ERROR_INVALID_RESPONSE = 5,      ///< response frame 或字段无效。
    GALAY_RPC_ERROR_REQUEST_TIMEOUT = 6,       ///< 请求超时。
    GALAY_RPC_ERROR_CONNECTION_CLOSED = 7,     ///< 连接已关闭或不可用。
    GALAY_RPC_ERROR_SERIALIZATION_ERROR = 8,   ///< 序列化失败。
    GALAY_RPC_ERROR_DESERIALIZATION_ERROR = 9, ///< 反序列化失败。
    GALAY_RPC_ERROR_INTERNAL_ERROR = 10,       ///< 内部错误。
    GALAY_RPC_ERROR_CANCELLED = 11,            ///< 调用已取消。
    GALAY_RPC_ERROR_DEADLINE_EXCEEDED = 12,    ///< deadline 超时。
    GALAY_RPC_ERROR_RESOURCE_EXHAUSTED = 13,   ///< 资源耗尽。
    GALAY_RPC_ERROR_RATE_LIMITED = 14,         ///< 被限流。
    GALAY_RPC_ERROR_CIRCUIT_OPEN = 15,         ///< 熔断打开。
    GALAY_RPC_ERROR_UNAUTHENTICATED = 16,      ///< 未认证。
    GALAY_RPC_ERROR_PERMISSION_DENIED = 17,    ///< 无权限。
    GALAY_RPC_ERROR_UNAVAILABLE = 18           ///< 服务不可用。
} galay_rpc_error_code_t;

/**
 * @brief RPC request frame 的 C 视图。
 * @details encode 时 service/method/payload 由调用方借用传入，仅在调用期间读取；
 * decode 时这些指针借用输入 frame buffer，输入 buffer 必须比 request 视图更长寿。
 */
typedef struct galay_rpc_request_t {
    uint32_t request_id;              ///< 请求 ID，由调用方或 client 分配。
    galay_rpc_call_mode_t call_mode;  ///< 调用模式。
    galay_bool_t end_of_stream;       ///< 当前 frame 是否结束该方向的流。
    const char* service;              ///< 借用 service 名称，不要求 NUL 结尾。
    size_t service_len;               ///< service 字节数。
    const char* method;               ///< 借用 method 名称，不要求 NUL 结尾。
    size_t method_len;                ///< method 字节数。
    const void* payload;              ///< 借用 payload；`payload_len == 0` 时可为 NULL。
    size_t payload_len;               ///< payload 字节数。
} galay_rpc_request_t;

/**
 * @brief RPC response frame 的 C 视图。
 * @details encode 时 payload 仅在调用期间借用；decode 时 payload 借用输入 frame buffer。
 */
typedef struct galay_rpc_response_t {
    uint32_t request_id;                    ///< 对应 request ID。
    galay_rpc_call_mode_t call_mode;        ///< 调用模式。
    galay_bool_t end_of_stream;             ///< 当前 frame 是否结束该方向的流。
    galay_rpc_error_code_t error_code;      ///< RPC 业务错误码。
    const void* payload;                    ///< 借用 payload；`payload_len == 0` 时可为 NULL。
    size_t payload_len;                     ///< payload 字节数。
} galay_rpc_response_t;

/**
 * @brief RPC client handle。
 * @details 拥有一个 TCP socket；connect/call/stream/close API 应在同一 C runtime/coroutine
 * 调度上下文内顺序调用，不提供跨线程同步。
 */
typedef struct galay_rpc_client_t galay_rpc_client_t;

/**
 * @brief RPC server handle。
 * @details 拥有 listener socket，并借用已注册 service 指针；service 必须比 server
 * 注册关系更长寿，或在 destroy server 前保持有效。
 */
typedef struct galay_rpc_server_t galay_rpc_server_t;

/**
 * @brief RPC service handle。
 * @details 拥有 method 路由表；回调 userdata 生命周期由调用方管理。
 */
typedef struct galay_rpc_service_t galay_rpc_service_t;

/**
 * @brief RPC client stream handle。
 * @details 借用 client；client 必须比 stream 更长寿。stream 关闭或 destroy 后不得继续读写。
 */
typedef struct galay_rpc_stream_t galay_rpc_stream_t;

/**
 * @brief RPC 取消源 handle。
 * @details 内部为原子取消标记；可在调用前传入 call options。当前 unary call 只在发送前
 * 检查取消，不中断已经进入 kernel TCP I/O 的等待。
 */
typedef struct galay_rpc_cancellation_source_t galay_rpc_cancellation_source_t;

/**
 * @brief RPC 逻辑连接池 handle。
 * @details 当前 C ABI pool 管理每 endpoint 的 lease 计数和 ID，不直接暴露 TCP client。
 */
typedef struct galay_rpc_pool_t galay_rpc_pool_t;

/**
 * @brief RPC pool lease handle。
 * @details 由 acquire 返回，必须通过 `galay_rpc_pool_release` 归还；release 后句柄失效。
 */
typedef struct galay_rpc_pool_lease_t galay_rpc_pool_lease_t;

/**
 * @brief RPC client 配置。
 * @details create 时复制 host 字符串；NULL 或空 host 使用默认值。
 */
typedef struct galay_rpc_client_config_t {
    const char* host;             ///< 远端地址；NULL 或空字符串时默认 127.0.0.1。
    uint16_t port;                ///< 远端端口；0 表示无效。
    int64_t connect_timeout_ms;   ///< connect 默认超时；负数表示无限等待。
} galay_rpc_client_config_t;

/**
 * @brief RPC server 配置。
 * @details create 时复制 host 字符串；port 为 0 时由系统分配，可通过 local_endpoint 查询。
 */
typedef struct galay_rpc_server_config_t {
    const char* host;             ///< 监听地址；NULL 或空字符串时默认 127.0.0.1。
    uint16_t port;                ///< 监听端口；0 表示由系统分配。
    int backlog;                  ///< listen backlog；<=0 时使用默认值 128。
} galay_rpc_server_config_t;

/**
 * @brief RPC 调用选项。
 * @details options 指针只在调用期间借用；cancellation 指针必须在调用返回前保持有效。
 */
typedef struct galay_rpc_call_options_t {
    int64_t timeout_ms;                                      ///< 每次 socket I/O 超时。
    const galay_rpc_cancellation_source_t* cancellation;     ///< 可选取消源；调用期间必须有效。
} galay_rpc_call_options_t;

/**
 * @brief RPC response 拥有型 buffer。
 * @details client call/stream read 成功时会复制 response payload 到该结构；
 * 调用方必须调用 `galay_rpc_response_buffer_destroy` 释放 payload。
 */
typedef struct galay_rpc_response_buffer_t {
    uint32_t request_id;                    ///< 对应 request ID。
    galay_rpc_call_mode_t call_mode;        ///< 调用模式。
    galay_bool_t end_of_stream;             ///< 是否为流结束响应。
    galay_rpc_error_code_t error_code;      ///< RPC 业务错误码。
    void* payload;       ///< 成功时由 C ABI 分配；调用方用 galay_rpc_response_buffer_destroy 释放。
    size_t payload_len;                     ///< payload 字节数。
} galay_rpc_response_buffer_t;

/**
 * @brief RPC pool 配置。
 * @details max 为 0 时规范化为 1；min 大于 max 时规范化为 max。
 */
typedef struct galay_rpc_pool_config_t {
    size_t min_connections_per_endpoint; ///< 每 endpoint 预留 lease 数。
    size_t max_connections_per_endpoint; ///< 每 endpoint 最大 lease 数。
} galay_rpc_pool_config_t;

/**
 * @brief C RPC 方法处理器。
 * @details 回调在 server coroutine 中同步执行；request 的指针和 payload 仅在回调期间有效。
 * response 的 payload 可借用用户内存，server 会在回调返回后、发送完成前同步读取。
 * @param request 借用 request 视图，不得保存。
 * @param response 输出 response；handler 应设置 payload、payload_len、end_of_stream 等字段。
 * @param user_data 注册 method 时传入的用户指针。
 * @return `GALAY_RPC_ERROR_OK` 表示业务成功；非 OK 会作为 response error_code 返回给 client。
 * @note handler 不应抛异常；可恢复错误应通过返回的 RPC error code 表达。
 */
typedef galay_rpc_error_code_t (*galay_rpc_method_handler_fn)(
    const galay_rpc_request_t* request,
    galay_rpc_response_t* response,
    void* user_data);

/**
 * @brief 返回 RPC 错误码字符串。
 * @param code RPC 错误码。
 * @return 永久有效的静态字符串；未知值返回 "Unknown"。
 */
const char* galay_rpc_error_string(galay_rpc_error_code_t code);

/**
 * @brief 将 RPC 错误码映射为通用 C 状态码。
 * @param code RPC 错误码。
 * @return 对应 `galay_status_t`。
 */
galay_status_t galay_rpc_error_to_status(galay_rpc_error_code_t code);

/**
 * @brief 校验 service/method 名称。
 * @param name 名称 buffer。
 * @param name_len 名称字节数。
 * @return 非空、长度 <= 65535 且只含字母数字、下划线或点时返回 `GALAY_TRUE`。
 */
galay_bool_t galay_rpc_name_is_valid(const char* name, size_t name_len);

/**
 * @brief 计算 request 编码后字节数。
 * @param request request 视图。
 * @param size 输出所需字节数。
 * @return 参数或字段无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_request_encoded_size(const galay_rpc_request_t* request, size_t* size);

/**
 * @brief 计算 response 编码后字节数。
 * @param response response 视图。
 * @param size 输出所需字节数。
 * @return 参数或字段无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_response_encoded_size(const galay_rpc_response_t* response, size_t* size);

/**
 * @brief 编码 request frame。
 * @param request request 视图；service/method/payload 在调用期间借用。
 * @param out 输出 buffer。
 * @param out_len 输出 buffer 容量。
 * @param written 成功时写入实际字节数。
 * @return buffer 不足返回 `GALAY_OUT_OF_MEMORY`；字段非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_encode_request(const galay_rpc_request_t* request, uint8_t* out,
                                        size_t out_len, size_t* written);

/**
 * @brief 编码 response frame。
 * @param response response 视图；payload 在调用期间借用。
 * @param out 输出 buffer。
 * @param out_len 输出 buffer 容量。
 * @param written 成功时写入实际字节数。
 * @return buffer 不足返回 `GALAY_OUT_OF_MEMORY`；字段非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_encode_response(const galay_rpc_response_t* response, uint8_t* out,
                                         size_t out_len, size_t* written);

/**
 * @brief 解码 request frame。
 * @param data 输入 frame buffer。
 * @param data_len 输入字节数。
 * @param out 输出 request 视图。
 * @param consumed 成功时输出消费字节数。
 * @param rpc_error 输出 RPC 反序列化错误码。
 * @return 成功返回 `GALAY_OK`；截断或非法 frame 返回 `GALAY_PROTOCOL_ERROR`。
 * @note out 中 service/method/payload 借用 data，data 必须在 out 使用期间保持有效。
 */
galay_status_t galay_rpc_decode_request(const uint8_t* data, size_t data_len,
                                        galay_rpc_request_t* out, size_t* consumed,
                                        galay_rpc_error_code_t* rpc_error);

/**
 * @brief 解码 response frame。
 * @param data 输入 frame buffer。
 * @param data_len 输入字节数。
 * @param out 输出 response 视图。
 * @param consumed 成功时输出消费字节数。
 * @param rpc_error 输出 RPC 反序列化错误码。
 * @return 成功返回 `GALAY_OK`；截断或非法 frame 返回 `GALAY_PROTOCOL_ERROR`。
 * @note out 中 payload 借用 data，data 必须在 out 使用期间保持有效。
 */
galay_status_t galay_rpc_decode_response(const uint8_t* data, size_t data_len,
                                         galay_rpc_response_t* out, size_t* consumed,
                                         galay_rpc_error_code_t* rpc_error);

/**
 * @brief 返回默认 client 配置。
 * @return 默认 host 为 127.0.0.1，port 为 0，connect timeout 为 -1。
 */
galay_rpc_client_config_t galay_rpc_client_config_default(void);

/**
 * @brief 返回默认 server 配置。
 * @return 默认 host 为 127.0.0.1，port 为 0，backlog 为 128。
 */
galay_rpc_server_config_t galay_rpc_server_config_default(void);

/**
 * @brief 返回默认调用选项。
 * @return timeout 为 -1，cancellation 为 NULL。
 */
galay_rpc_call_options_t galay_rpc_call_options_default(void);

/**
 * @brief 返回默认 pool 配置。
 * @return min 为 0，max 为 1。
 */
galay_rpc_pool_config_t galay_rpc_pool_config_default(void);

/**
 * @brief 创建 RPC client。
 * @param config 可为 NULL；NULL 使用默认配置。host 会被复制。
 * @param out 成功时返回 client；调用方用 `galay_rpc_client_destroy` 释放。
 * @return `GALAY_OK`、`GALAY_INVALID_ARGUMENT` 或 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_rpc_client_create(const galay_rpc_client_config_t* config,
                                       galay_rpc_client_t** out);

/**
 * @brief 销毁 RPC client。
 * @param client 可为 NULL；会释放其拥有的 socket handle。
 * @note 不会自动 destroy 仍借用该 client 的 stream；调用方应先关闭并销毁 stream。
 */
void galay_rpc_client_destroy(galay_rpc_client_t* client);

/**
 * @brief 连接 RPC server。
 * @param client RPC client。
 * @param timeout_ms connect 超时；负数时可使用 config connect_timeout_ms。
 * @return `C_IOResultOk` 表示连接成功且 `ptr` 指向 client；错误通过 `C_IOResult` 返回。
 * @note 使用 C coroutine TCP connect；应在 galay C runtime/coroutine 上下文内调用。
 */
C_IOResult galay_rpc_client_connect(galay_rpc_client_t* client, int64_t timeout_ms);

/**
 * @brief 关闭 RPC client socket。
 * @param client RPC client。
 * @param timeout_ms close 超时。
 * @return 成功返回 `C_IOResultOk`；参数或 I/O 失败显式返回。
 * @note close 不销毁 client handle。
 */
C_IOResult galay_rpc_client_close(galay_rpc_client_t* client, int64_t timeout_ms);

/**
 * @brief 发送 heartbeat 并等待 heartbeat 响应。
 * @param client 已连接 RPC client。
 * @param timeout_ms send/recv 超时。
 * @return 成功返回 `C_IOResultOk`，`value` 为 request id；协议错误返回 `C_IOResultError`。
 */
C_IOResult galay_rpc_client_heartbeat(galay_rpc_client_t* client, int64_t timeout_ms);

/**
 * @brief 发起 unary RPC 调用。
 * @param client 已连接 RPC client。
 * @param service service 名称 buffer。
 * @param service_len service 字节数。
 * @param method method 名称 buffer。
 * @param method_len method 字节数。
 * @param payload 请求 payload；`payload_len == 0` 时可为 NULL。
 * @param payload_len payload 字节数。
 * @param timeout_ms 每次 socket I/O 超时。
 * @param out_response 输出拥有型 response buffer，调用方必须 destroy。
 * @return 成功读取 response frame 返回 `C_IOResultOk`；RPC 业务错误写入 out_response.error_code。
 */
C_IOResult galay_rpc_client_call(galay_rpc_client_t* client,
                                 const char* service,
                                 size_t service_len,
                                 const char* method,
                                 size_t method_len,
                                 const void* payload,
                                 size_t payload_len,
                                 int64_t timeout_ms,
                                 galay_rpc_response_buffer_t* out_response);

/**
 * @brief 使用调用选项发起 unary RPC 调用。
 * @param client 已连接 RPC client。
 * @param service service 名称 buffer。
 * @param service_len service 字节数。
 * @param method method 名称 buffer。
 * @param method_len method 字节数。
 * @param payload 请求 payload；`payload_len == 0` 时可为 NULL。
 * @param payload_len payload 字节数。
 * @param options 可为 NULL；调用期间借用。
 * @param out_response 输出拥有型 response buffer，调用方必须 destroy。
 * @return 取消、超时、I/O 或协议错误通过 `C_IOResult` 返回。
 * @note cancellation 只在发送前检查；不会异步打断已经挂起的 socket I/O。
 */
C_IOResult galay_rpc_client_call_with_options(galay_rpc_client_t* client,
                                              const char* service,
                                              size_t service_len,
                                              const char* method,
                                              size_t method_len,
                                              const void* payload,
                                              size_t payload_len,
                                              const galay_rpc_call_options_t* options,
                                              galay_rpc_response_buffer_t* out_response);

/**
 * @brief 创建 RPC server。
 * @param config 可为 NULL；NULL 使用默认配置。host 会被复制。
 * @param out 成功时返回 server；调用方 destroy。
 * @return `GALAY_OK` 或错误状态。
 */
galay_status_t galay_rpc_server_create(const galay_rpc_server_config_t* config,
                                       galay_rpc_server_t** out);

/**
 * @brief 销毁 RPC server。
 * @param server 可为 NULL；会释放 listener handle。
 * @note server 只借用已注册 service，不销毁 service。
 */
void galay_rpc_server_destroy(galay_rpc_server_t* server);

/**
 * @brief 绑定并监听 RPC server endpoint。
 * @param server RPC server。
 * @return 成功返回 `GALAY_OK`；bind/listen 失败映射为 `galay_status_t`。
 */
galay_status_t galay_rpc_server_listen(galay_rpc_server_t* server);

/**
 * @brief 获取 server 实际监听 endpoint。
 * @param server 已 listen 的 server。
 * @param out 输出 C_Host。
 * @return 参数或 socket 状态无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_server_local_endpoint(galay_rpc_server_t* server, C_Host* out);

/**
 * @brief 注册 service 到 server。
 * @param server RPC server。
 * @param service service handle；server 借用该指针。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note service 必须在 server 使用期间保持有效。
 */
galay_status_t galay_rpc_server_register_service(galay_rpc_server_t* server,
                                                 galay_rpc_service_t* service);

/**
 * @brief 接受并服务一个 RPC TCP 连接。
 * @param server 已 listen 的 server。
 * @param timeout_ms accept/read/write/close 超时。
 * @return 连接处理成功返回 `C_IOResultOk`；I/O、协议或参数错误通过 `C_IOResult` 返回。
 * @note 在当前 C coroutine 内循环处理该连接上的 frame，连接 EOF 后关闭 session。
 */
C_IOResult galay_rpc_server_serve_one(galay_rpc_server_t* server, int64_t timeout_ms);

/**
 * @brief 创建 RPC service。
 * @param name service 名称 buffer。
 * @param name_len service 字节数。
 * @param out 成功时返回 service；调用方 destroy。
 * @return 名称非法或分配失败通过 `galay_status_t` 返回。
 */
galay_status_t galay_rpc_service_create(const char* name,
                                        size_t name_len,
                                        galay_rpc_service_t** out);

/**
 * @brief 销毁 RPC service。
 * @param service 可为 NULL；销毁后不得继续被 server 使用。
 */
void galay_rpc_service_destroy(galay_rpc_service_t* service);

/**
 * @brief 注册 unary method。
 * @param service RPC service。
 * @param method method 名称 buffer。
 * @param method_len method 字节数。
 * @param handler method handler，不能为空。
 * @param user_data 透传给 handler。
 * @return 成功返回 `GALAY_OK`。
 */
galay_status_t galay_rpc_service_register_unary(galay_rpc_service_t* service,
                                                const char* method,
                                                size_t method_len,
                                                galay_rpc_method_handler_fn handler,
                                                void* user_data);

/**
 * @brief 注册 streaming method。
 * @param service RPC service。
 * @param method method 名称 buffer。
 * @param method_len method 字节数。
 * @param mode streaming 调用模式，不能为 unary。
 * @param handler method handler，不能为空。
 * @param user_data 透传给 handler。
 * @return mode、名称或 handler 无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_service_register_streaming(galay_rpc_service_t* service,
                                                    const char* method,
                                                    size_t method_len,
                                                    galay_rpc_call_mode_t mode,
                                                    galay_rpc_method_handler_fn handler,
                                                    void* user_data);

/**
 * @brief 打开 client stream。
 * @param client 已连接 RPC client。
 * @param service service 名称 buffer。
 * @param service_len service 字节数。
 * @param method method 名称 buffer。
 * @param method_len method 字节数。
 * @param mode streaming 模式。
 * @param out_stream 成功时返回 stream；调用方用 `galay_rpc_stream_destroy` 释放。
 * @return 成功返回 `C_IOResultOk`，`ptr` 指向 stream。
 * @note stream 借用 client；client close/destroy 前应先 close/destroy stream。
 */
C_IOResult galay_rpc_client_stream_open(galay_rpc_client_t* client,
                                        const char* service,
                                        size_t service_len,
                                        const char* method,
                                        size_t method_len,
                                        galay_rpc_call_mode_t mode,
                                        galay_rpc_stream_t** out_stream);

/**
 * @brief 向 stream 写入一个 request frame。
 * @param stream RPC stream。
 * @param payload payload buffer；`payload_len == 0` 时可为 NULL。
 * @param payload_len payload 字节数。
 * @param end_of_stream 是否结束客户端发送方向。
 * @param timeout_ms send 超时。
 * @return 成功返回 `C_IOResultOk`；stream 已关闭或 I/O 失败显式返回。
 */
C_IOResult galay_rpc_stream_write(galay_rpc_stream_t* stream,
                                  const void* payload,
                                  size_t payload_len,
                                  galay_bool_t end_of_stream,
                                  int64_t timeout_ms);

/**
 * @brief 从 stream 读取一个 response frame。
 * @param stream RPC stream。
 * @param timeout_ms recv 超时。
 * @param out_response 输出拥有型 response buffer，调用方 destroy。
 * @return 成功返回 `C_IOResultOk`；收到 end_of_stream 会将 stream 标记为关闭。
 */
C_IOResult galay_rpc_stream_read(galay_rpc_stream_t* stream,
                                 int64_t timeout_ms,
                                 galay_rpc_response_buffer_t* out_response);

/**
 * @brief 发送空 payload 的 end_of_stream frame 并关闭 stream。
 * @param stream RPC stream。
 * @param timeout_ms send 超时。
 * @return 已关闭 stream 返回 `C_IOResultOk`；参数或 I/O 失败显式返回。
 */
C_IOResult galay_rpc_stream_close(galay_rpc_stream_t* stream, int64_t timeout_ms);

/**
 * @brief 销毁 stream handle。
 * @param stream 可为 NULL；不会自动关闭 client socket。
 */
void galay_rpc_stream_destroy(galay_rpc_stream_t* stream);

/**
 * @brief 创建取消源。
 * @param out 成功时返回 cancellation source；调用方 destroy。
 * @return `GALAY_OK`、`GALAY_INVALID_ARGUMENT` 或 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_rpc_cancellation_source_create(galay_rpc_cancellation_source_t** out);

/**
 * @brief 标记取消源已取消。
 * @param source cancellation source，可为 NULL。
 * @note 可从其他线程设置原子标记；当前 call 仅在进入 I/O 前观察该标记。
 */
void galay_rpc_cancellation_source_cancel(galay_rpc_cancellation_source_t* source);

/**
 * @brief 销毁取消源。
 * @param source 可为 NULL；不得在仍有调用借用该 source 时销毁。
 */
void galay_rpc_cancellation_source_destroy(galay_rpc_cancellation_source_t* source);

/**
 * @brief 释放 response buffer 内部 payload。
 * @param response response buffer，可为 NULL。
 * @note 本函数会 free payload 并把结构重置为 OK/unary/空 payload。
 */
void galay_rpc_response_buffer_destroy(galay_rpc_response_buffer_t* response);

/**
 * @brief 创建 RPC pool。
 * @param config 可为 NULL；NULL 使用默认 pool 配置。
 * @param out 成功时返回 pool；调用方 destroy。
 * @return `GALAY_OK` 或错误状态。
 * @note pool 不提供跨线程同步；应在同一调度上下文内 acquire/release/shutdown。
 */
galay_status_t galay_rpc_pool_create(const galay_rpc_pool_config_t* config,
                                     galay_rpc_pool_t** out);

/**
 * @brief 销毁 RPC pool。
 * @param pool 可为 NULL；调用方应先 release 所有 lease。
 */
void galay_rpc_pool_destroy(galay_rpc_pool_t* pool);

/**
 * @brief 确保 endpoint 在 pool 中存在并按 min 配置预留 lease。
 * @param pool RPC pool。
 * @param host endpoint host，不能为空。
 * @param port endpoint port，不能为 0。
 * @return pool shutdown 返回 `GALAY_IO_ERROR`；参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_pool_ensure_endpoint(galay_rpc_pool_t* pool,
                                              const char* host,
                                              uint16_t port);

/**
 * @brief 获取 endpoint lease。
 * @param pool RPC pool。
 * @param host endpoint host，不能为空。
 * @param port endpoint port，不能为 0。
 * @param out_lease 成功时返回 lease；必须通过 `galay_rpc_pool_release` 归还。
 * @return endpoint 满额返回 `GALAY_OUT_OF_MEMORY`；pool shutdown 返回 `GALAY_IO_ERROR`。
 */
galay_status_t galay_rpc_pool_acquire(galay_rpc_pool_t* pool,
                                      const char* host,
                                      uint16_t port,
                                      galay_rpc_pool_lease_t** out_lease);

/**
 * @brief 归还 endpoint lease。
 * @param pool RPC pool。
 * @param lease acquire 返回的 lease。
 * @param broken `GALAY_TRUE` 表示该 lease 对应连接不可复用，会减少 total 计数。
 * @return 成功返回 `GALAY_OK`；重复 release 或 pool 不匹配返回错误。
 * @note release 会销毁 lease handle，调用方不得再次访问。
 */
galay_status_t galay_rpc_pool_release(galay_rpc_pool_t* pool,
                                      galay_rpc_pool_lease_t* lease,
                                      galay_bool_t broken);

/**
 * @brief 关闭 pool 并清空所有 endpoint 计数。
 * @param pool RPC pool。
 * @return 成功返回 `GALAY_OK`。
 * @note shutdown 后新的 ensure/acquire 会返回 `GALAY_IO_ERROR`；已借出的 lease 应停止使用。
 */
galay_status_t galay_rpc_pool_shutdown(galay_rpc_pool_t* pool);

/**
 * @brief 查询 endpoint 可用 lease 数。
 * @param pool RPC pool。
 * @param host endpoint host。
 * @param port endpoint port。
 * @param out_count 输出可用数量；失败时置 0。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_pool_available_count(const galay_rpc_pool_t* pool,
                                              const char* host,
                                              uint16_t port,
                                              size_t* out_count);

/**
 * @brief 查询 endpoint 占用 lease 数。
 * @param pool RPC pool。
 * @param host endpoint host。
 * @param port endpoint port。
 * @param out_count 输出占用数量；失败时置 0。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_pool_in_use_count(const galay_rpc_pool_t* pool,
                                           const char* host,
                                           uint16_t port,
                                           size_t* out_count);

/**
 * @brief 获取 lease ID。
 * @param lease pool lease。
 * @param out_id 输出 lease ID；失败时置 0。
 * @return lease 无效或已 release 返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_rpc_pool_lease_id(const galay_rpc_pool_lease_t* lease, uint64_t* out_id);

#ifdef __cplusplus
}
#endif

#endif
