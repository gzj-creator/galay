/**
 * @file http2_c.h
 * @brief HTTP/2 C ABI。
 * @details
 * 该头文件暴露 h2c client/server、connection、stream、frame 和 header
 * buffer 的 C 接口。所有 handle 均为不透明类型；除明确标注为借用的返回值外，
 * 成功创建或解码得到的对象都必须由调用方通过对应 destroy 函数释放。
 *
 * I/O 接口返回 `C_IOResult`。`code` 表示通用 I/O 状态；HTTP/2 运行时错误会
 * 写入 `value`，调用方可将其转换为 `galay_http2_error_code_t` 后传给
 * `galay_http2_error_code_get_error`。`bytes` 表示本次成功读写的 payload
 * 字节数或底层帧字节数，具体见各函数说明。
 *
 * @note 本 C ABI 不提供内部同步。除非外层自行串行化访问，否则同一 client、
 * server、connection 或 stream 不能被多个线程/协程并发操作。I/O 函数遵循
 * 底层 TCP C coroutine 约束：等待网络事件时挂起当前 C coroutine；不应在未
 * 运行 galay C runtime 的上下文中调用需要网络等待的函数。
 */
#ifndef GALAY_C_HTTP2_HTTP2_C_H
#define GALAY_C_HTTP2_HTTP2_C_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP/2 frame header 固定长度。
 * @details 所有 HTTP/2 帧均使用 9 字节 frame header。
 */
#define GALAY_HTTP2_FRAME_HEADER_LENGTH 9u

/**
 * @brief 当前 C ABI 支持的 HTTP/2 帧类型。
 * @details 未列出的帧类型不会被当前实现构造；解码未知或不满足连接/流约束的帧
 * 会返回协议错误。
 */
typedef enum galay_http2_frame_type_t {
    GALAY_HTTP2_FRAME_DATA = 0,          ///< DATA 帧，必须绑定非 0 stream id。
    GALAY_HTTP2_FRAME_HEADERS = 1,       ///< HEADERS 帧，必须绑定非 0 stream id。
    GALAY_HTTP2_FRAME_RST_STREAM = 3,    ///< RST_STREAM 帧，payload 为 4 字节错误码。
    GALAY_HTTP2_FRAME_SETTINGS = 4,      ///< SETTINGS 帧，必须是 connection 级帧。
    GALAY_HTTP2_FRAME_PING = 6,          ///< PING 帧，payload 固定 8 字节。
    GALAY_HTTP2_FRAME_GOAWAY = 7,        ///< GOAWAY 帧，payload 至少包含 last stream id 与错误码。
    GALAY_HTTP2_FRAME_WINDOW_UPDATE = 8  ///< WINDOW_UPDATE 帧，increment 必须非 0。
} galay_http2_frame_type_t;

/**
 * @brief 当前 C ABI 支持校验和发送的 SETTINGS id。
 * @details `galay_http2_settings_value_validate` 会按 RFC 边界校验这些值。
 */
typedef enum galay_http2_settings_id_t {
    GALAY_HTTP2_SETTINGS_ENABLE_PUSH = 2,          ///< 值必须为 0 或 1。
    GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE = 4,  ///< 值不能超过 2^31 - 1。
    GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE = 5        ///< 值范围为 16384 到 16777215。
} galay_http2_settings_id_t;

/**
 * @brief HTTP/2 C ABI 写入 `C_IOResult.value` 的错误码。
 * @details
 * 对返回 `C_IOResult` 的 HTTP/2 API，`code != C_IOResultOk` 时 `value` 通常保存
 * 本枚举值。`C_IOResultTimeout` 会映射为 `GALAY_HTTP2_ERROR_TIMEOUT`，底层 TCP
 * 或 close/destroy 失败会映射为 `GALAY_HTTP2_ERROR_IO`，参数或状态非法通常映射为
 * `GALAY_HTTP2_ERROR_INTERNAL`。
 *
 * @note `GALAY_HTTP2_ERROR_CANCEL` 为 ABI 保留值；当前实现主要透传底层取消状态，
 * 调用方仍应优先检查 `C_IOResult.code`。
 */
typedef enum galay_http2_error_code_t {
    GALAY_HTTP2_ERROR_NONE = 0,           ///< 无 HTTP/2 运行时错误。
    GALAY_HTTP2_ERROR_PROTOCOL = 1,       ///< 帧格式、连接级约束或 HPACK/header 解析错误。
    GALAY_HTTP2_ERROR_INTERNAL = 2,       ///< 参数、状态、分配或内部边界错误。
    GALAY_HTTP2_ERROR_FLOW_CONTROL = 3,   ///< connection/stream flow-control window 越界。
    GALAY_HTTP2_ERROR_STREAM_CLOSED = 5,  ///< 在已关闭或不可收发的 stream 上操作。
    GALAY_HTTP2_ERROR_CANCEL = 8,         ///< 取消语义保留值。
    GALAY_HTTP2_ERROR_SETTINGS_ACK = 100, ///< SETTINGS ACK 相关保留值。
    GALAY_HTTP2_ERROR_STREAM_RESET = 101, ///< 收到 RST_STREAM 后继续读写该 stream。
    GALAY_HTTP2_ERROR_GOAWAY = 102,       ///< 连接收到 GOAWAY 后拒绝新 stream。
    GALAY_HTTP2_ERROR_IO = 103,           ///< 底层 socket I/O 失败或 EOF。
    GALAY_HTTP2_ERROR_TIMEOUT = 104       ///< 底层等待超时。
} galay_http2_error_code_t;

/**
 * @brief HTTP/2 stream 生命周期状态。
 * @details 状态由 HEADERS/DATA END_STREAM、RST_STREAM、GOAWAY 和 destroy 操作推进；
 * 调用方只能读取状态，不能直接修改。
 */
typedef enum galay_http2_stream_state_t {
    GALAY_HTTP2_STREAM_IDLE = 0,                ///< stream 尚未打开。
    GALAY_HTTP2_STREAM_OPEN = 1,                ///< 双向均可收发。
    GALAY_HTTP2_STREAM_HALF_CLOSED_LOCAL = 2,   ///< 本端已发送 END_STREAM，只能接收。
    GALAY_HTTP2_STREAM_HALF_CLOSED_REMOTE = 3,  ///< 对端已发送 END_STREAM，只能发送。
    GALAY_HTTP2_STREAM_CLOSED = 4               ///< stream 已关闭或 reset。
} galay_http2_stream_state_t;

/**
 * @brief HTTP/2 h2c client/server 配置。
 * @details
 * `galay_http2_client_create` 和 `galay_http2_server_create` 会复制 `host` 字符串内容，
 * 调用返回后调用方可释放或修改原始字符串。传入 NULL 配置或字段为 0/空时会使用
 * 默认值：host 为 127.0.0.1，port 为 0，initial window 为 65535，max frame size
 * 为 16384，max concurrent streams 为 100。
 */
typedef struct galay_http2_config_t {
    const char* host;                 ///< 监听或连接的 host；create 时复制。
    uint16_t port;                    ///< TCP 端口；server 传 0 表示由系统分配。
    uint32_t initial_window_size;     ///< 本端初始 flow-control window；0 表示默认值。
    uint32_t max_frame_size;          ///< 本端允许的最大 frame payload；0 表示默认值。
    uint32_t max_concurrent_streams;  ///< 本端允许的最大并发 stream；0 表示默认值。
} galay_http2_config_t;

/**
 * @brief HTTP/2 frame handle。
 * @details 由 create/decode 返回，调用方独占所有权并用 `galay_http2_frame_destroy` 释放。
 */
typedef struct galay_http2_frame_t galay_http2_frame_t;

/**
 * @brief HTTP/2 headers handle。
 * @details 保存 name/value 副本；作为入参时只在调用期间借用，作为出参时调用方获得所有权。
 */
typedef struct galay_http2_headers_t galay_http2_headers_t;

/**
 * @brief HTTP/2 client handle。
 * @details 独占其连接；destroy 会销毁仍由 client 持有的 connection 和 stream。
 */
typedef struct galay_http2_client_t galay_http2_client_t;

/**
 * @brief HTTP/2 server handle。
 * @details 独占 listener socket；已 accept 的 connection 不归 server destroy 管理。
 */
typedef struct galay_http2_server_t galay_http2_server_t;

/**
 * @brief HTTP/2 connection handle。
 * @details 由 client 内部拥有或由 server accept 返回给调用方；connection 拥有其 stream。
 */
typedef struct galay_http2_conn_t galay_http2_conn_t;

/**
 * @brief HTTP/2 stream handle。
 * @details stream 归所属 connection 管理；可由调用方用 `galay_http2_stream_destroy`
 * 提前释放，或随 connection destroy 释放。
 */
typedef struct galay_http2_stream_t galay_http2_stream_t;

/**
 * @brief 将通用 `galay_status_t` 转为可读字符串。
 * @param status 任意 galay C status。
 * @return 静态字符串；调用方不得释放。
 */
const char* galay_http2_get_error(galay_status_t status);

/**
 * @brief 将 HTTP/2 C ABI 错误码转为可读字符串。
 * @param error `C_IOResult.value` 中保存的 HTTP/2 错误码。
 * @return 静态字符串；调用方不得释放。
 */
const char* galay_http2_error_code_get_error(galay_http2_error_code_t error);

/**
 * @brief 校验 HTTP/2 stream id。
 * @details stream id 必须使用低 31 位；除 connection 级帧外，stream id 不能为 0。
 * @param stream_id 待校验 stream id。
 * @param allow_zero 是否允许 0，通常仅 connection 级帧传 `GALAY_TRUE`。
 * @return 合法返回 `GALAY_OK`；非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_http2_stream_id_validate(uint32_t stream_id, galay_bool_t allow_zero);

/**
 * @brief 校验 SETTINGS 值边界。
 * @param id SETTINGS 标识符。
 * @param value 对应值。
 * @return 合法返回 `GALAY_OK`；违反协议边界返回 `GALAY_PROTOCOL_ERROR`。
 */
galay_status_t galay_http2_settings_value_validate(galay_http2_settings_id_t id, uint32_t value);

/**
 * @brief 创建 PING frame。
 * @param opaque 8 字节 opaque data；调用期间借用并复制进 frame。
 * @param ack 是否设置 ACK flag。
 * @param out 成功时返回新 frame，调用方用 `galay_http2_frame_destroy` 释放。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`；分配失败返回 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_http2_ping_frame_create(const uint8_t opaque[8], galay_bool_t ack,
                                             galay_http2_frame_t** out);

/**
 * @brief 销毁 HTTP/2 frame。
 * @param frame 可为 NULL；非 NULL 时必须是本 C ABI 返回的 frame。
 */
void galay_http2_frame_destroy(galay_http2_frame_t* frame);

/**
 * @brief 编码 HTTP/2 frame 到调用方 buffer。
 * @param frame 待编码 frame。
 * @param out 输出 buffer；可为 NULL 用于查询所需长度。
 * @param out_len 入参为 `out` 容量，出参为实际所需/写入长度。
 * @return 成功返回 `GALAY_OK`；buffer 为空或容量不足时写入所需长度并返回
 * `GALAY_OUT_OF_MEMORY`；参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note `out` 由调用方拥有，函数不会保存该指针。
 */
galay_status_t galay_http2_frame_encode(const galay_http2_frame_t* frame, uint8_t* out,
                                        size_t* out_len);

/**
 * @brief 从连续字节解码一个 HTTP/2 frame。
 * @param data 完整 frame bytes，至少包含 9 字节 header 和 payload。
 * @param data_len `data` 字节数。
 * @param out 成功时返回新 frame；调用方用 `galay_http2_frame_destroy` 释放。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`；截断或协议约束失败返回
 * `GALAY_PROTOCOL_ERROR`；分配失败返回 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_http2_frame_decode(const uint8_t* data, size_t data_len,
                                        galay_http2_frame_t** out);

/**
 * @brief 获取 frame 类型。
 * @param frame frame handle；NULL 时返回 `GALAY_HTTP2_FRAME_DATA`。
 * @return frame 类型。
 */
galay_http2_frame_type_t galay_http2_frame_type(const galay_http2_frame_t* frame);

/**
 * @brief 获取 frame stream id。
 * @param frame frame handle；NULL 时返回 0。
 * @return 低 31 位 stream id。
 */
uint32_t galay_http2_frame_stream_id(const galay_http2_frame_t* frame);

/**
 * @brief 复制 PING frame 的 8 字节 opaque data。
 * @param frame PING frame handle。
 * @param out 调用方提供的 8 字节输出 buffer。
 * @return 成功返回 `GALAY_OK`；frame 非 PING、payload 长度错误或参数无效返回
 * `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_http2_ping_frame_opaque(const galay_http2_frame_t* frame, uint8_t out[8]);

/**
 * @brief 创建 headers 集合。
 * @param out 成功时返回 headers handle，调用方用 `galay_http2_headers_destroy` 释放。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`；分配失败返回 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_http2_headers_create(galay_http2_headers_t** out);

/**
 * @brief 销毁 headers 集合。
 * @param headers 可为 NULL；非 NULL 时必须是本 C ABI 返回的 headers。
 */
void galay_http2_headers_destroy(galay_http2_headers_t* headers);

/**
 * @brief 向 headers 集合追加一个 name/value。
 * @details `name` 和 `value` 会被复制进 headers；调用返回后原始字符串可释放。
 * @param headers headers handle。
 * @param name header name，不能为空字符串。
 * @param value header value，可为空字符串但不能为 NULL。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note HPACK/header 编码阶段要求 name/value 长度均不超过当前 C ABI 支持的 255 字节。
 */
galay_status_t galay_http2_headers_add(galay_http2_headers_t* headers, const char* name,
                                       const char* value);

/**
 * @brief 获取 headers 条目数量。
 * @param headers headers handle；NULL 时返回 0。
 * @return 条目数量。
 */
size_t galay_http2_headers_count(const galay_http2_headers_t* headers);

/**
 * @brief 按索引获取 header name/value 的借用指针。
 * @param headers headers handle。
 * @param index 条目索引。
 * @param name 成功时返回内部 name 指针。
 * @param value 成功时返回内部 value 指针。
 * @return 成功返回 `GALAY_OK`；参数无效或越界返回 `GALAY_INVALID_ARGUMENT`。
 * @note `name`/`value` 指向 headers 内部存储，只在 headers 未销毁且未被修改前有效；
 * 调用方不得释放或写入这些指针。
 */
galay_status_t galay_http2_headers_get(const galay_http2_headers_t* headers, size_t index,
                                       const char** name, const char** value);

/**
 * @brief 编码 headers 为当前 C ABI 使用的 HTTP/2 header block。
 * @param headers 待编码 headers。
 * @param out 输出 buffer；可为 NULL 用于查询所需长度。
 * @param out_len 入参为 `out` 容量，出参为实际所需/写入长度。
 * @return 成功返回 `GALAY_OK`；buffer 为空或容量不足时写入所需长度并返回
 * `GALAY_OUT_OF_MEMORY`；参数或 header 长度非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note `out` 由调用方拥有，函数不会保存该指针；编码结果可用于
 * `galay_http2_hpack_decode` 或 HTTP/2 HEADERS 发送路径。
 */
galay_status_t galay_http2_hpack_encode(const galay_http2_headers_t* headers, uint8_t* out,
                                        size_t* out_len);

/**
 * @brief 解码当前 C ABI 支持的 HTTP/2 header block。
 * @param data header block bytes；调用期间借用，不能为 NULL。
 * @param data_len `data` 字节数，允许为 0。
 * @param out 成功时返回 headers handle，调用方用 `galay_http2_headers_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；格式非法返回
 * `GALAY_PROTOCOL_ERROR`；分配失败返回 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_http2_hpack_decode(const uint8_t* data, size_t data_len,
                                        galay_http2_headers_t** out);

/**
 * @brief 返回 h2c C runtime 默认配置。
 * @details 返回值中的 `host` 指向静态字符串，调用方不得释放。
 * @return 默认监听/连接到 127.0.0.1:0，初始窗口 65535，最大帧 16384，
 * 最大并发 stream 为 100。
 */
galay_http2_config_t galay_http2_config_default(void);

/**
 * @brief 创建 HTTP/2 h2c client。
 * @param config client 配置；NULL 使用默认配置，`host` 会被复制。
 * @param out 成功时返回 client，调用方用 `galay_http2_client_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；分配失败返回
 * `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_http2_client_create(const galay_http2_config_t* config,
                                         galay_http2_client_t** out);

/**
 * @brief 销毁 client 及其拥有的 connection。
 * @param client 可为 NULL；非 NULL 时必须停止继续使用从该 client 借出的 connection/stream。
 * @note 若 client 仍持有 connection，destroy 会级联销毁 connection 和其 stream；
 * 调用方不得再单独 destroy `galay_http2_client_conn` 返回的借用指针。
 */
void galay_http2_client_destroy(galay_http2_client_t* client);

/**
 * @brief 在当前 C coroutine 中连接 h2c server 并完成 HTTP/2 client preface/SETTINGS 交换。
 * @param client 由 `galay_http2_client_create` 创建且尚未连接的 client。
 * @param timeout_ms 传递给每次底层 TCP I/O 的 timeout；不是整个握手的绝对 deadline。
 * @return 成功返回 `C_IOResultOk`；超时返回 `C_IOResultTimeout` 且 `value` 为
 * `GALAY_HTTP2_ERROR_TIMEOUT`；协议、I/O 或状态错误通过 `C_IOResult` 和 `value` 返回。
 * @note 等待底层 TCP 事件时会挂起当前 C coroutine；同一 client 不支持并发 connect。
 */
C_IOResult galay_http2_client_connect(galay_http2_client_t* client, int64_t timeout_ms);

/**
 * @brief 获取 client 当前 connection 的借用指针。
 * @param client client handle。
 * @return 已连接时返回 connection；未连接或参数为 NULL 时返回 NULL。
 * @note 返回指针仍由 client 拥有，生命周期到 client destroy 或内部连接销毁为止。
 */
galay_http2_conn_t* galay_http2_client_conn(galay_http2_client_t* client);

/**
 * @brief 在 client connection 上打开一个本端发起的 stream 并发送 HEADERS。
 * @param client 已连接 client。
 * @param headers 要发送的 headers；调用期间借用，不接管所有权。
 * @param end_stream 是否在 HEADERS 上携带 END_STREAM。
 * @param out_stream 可为 NULL；非 NULL 时成功返回 stream 借用指针。
 * @param timeout_ms 传递给每次底层 TCP I/O 的 timeout。
 * @return 成功返回 `C_IOResultOk`；GOAWAY、协议/编码错误、flow-control 或 I/O 错误通过
 * `C_IOResult` 和 `value` 返回。
 * @note 返回 stream 归 connection 管理，可用 `galay_http2_stream_destroy` 提前释放；
 * 同一 connection 上的新建 stream 和读写操作需要外层串行化。
 */
C_IOResult galay_http2_client_open_stream(galay_http2_client_t* client,
                                          const galay_http2_headers_t* headers,
                                          galay_bool_t end_stream,
                                          galay_http2_stream_t** out_stream,
                                          int64_t timeout_ms);

/**
 * @brief 创建 HTTP/2 h2c server。
 * @param config server 配置；NULL 使用默认配置，`host` 会被复制。
 * @param out 成功时返回 server，调用方用 `galay_http2_server_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；分配失败返回
 * `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_http2_server_create(const galay_http2_config_t* config,
                                         galay_http2_server_t** out);

/**
 * @brief 销毁 server listener。
 * @param server 可为 NULL。
 * @note 该函数只销毁 server 拥有的 listener；已经由 `galay_http2_server_accept`
 * 返回的 connection 由调用方负责 `galay_http2_conn_destroy`。
 */
void galay_http2_server_destroy(galay_http2_server_t* server);

/**
 * @brief 绑定并监听 h2c server socket。
 * @param server 尚未 listen 的 server。
 * @param out_port 可为 NULL；非 NULL 时写入实际监听端口。
 * @return 成功返回 `C_IOResultOk`；参数/状态非法返回 `C_IOResultInvalid`；socket
 * 创建、bind、listen 或 endpoint 查询失败返回 `C_IOResultError`，`value` 为
 * `GALAY_HTTP2_ERROR_IO` 或 `GALAY_HTTP2_ERROR_INTERNAL`。
 */
C_IOResult galay_http2_server_listen(galay_http2_server_t* server, uint16_t* out_port);

/**
 * @brief accept 一个 TCP 连接并完成 HTTP/2 server preface/SETTINGS 交换。
 * @param server 已 listen 的 server。
 * @param out_conn 成功时返回 connection，调用方用 `galay_http2_conn_destroy` 释放。
 * @param timeout_ms 传递给 accept 和每次底层 TCP I/O 的 timeout。
 * @return 成功返回 `C_IOResultOk`；超时、协议、I/O 或状态错误通过 `C_IOResult`
 * 和 `value` 返回。
 * @note 等待 accept/读写时挂起当前 C coroutine；server destroy 不会释放已返回的 connection。
 */
C_IOResult galay_http2_server_accept(galay_http2_server_t* server,
                                     galay_http2_conn_t** out_conn,
                                     int64_t timeout_ms);

/**
 * @brief 停止 server listener。
 * @param server server handle。
 * @param timeout_ms 当前实现仅校验 `timeout_ms >= -1`，关闭 listener 为立即 destroy。
 * @return 成功返回 `C_IOResultOk`；参数非法返回 `C_IOResultInvalid`；底层 destroy
 * 失败返回 `C_IOResultError` 且 `value` 为 `GALAY_HTTP2_ERROR_IO`。
 * @note 不会关闭已经 accept 出去的 connection。
 */
C_IOResult galay_http2_server_stop(galay_http2_server_t* server, int64_t timeout_ms);

/**
 * @brief 销毁 HTTP/2 connection 及其仍存活的 stream。
 * @param conn connection handle；不能为 NULL。
 * @return 成功返回 `GALAY_OK`；NULL 返回 `GALAY_INVALID_ARGUMENT`；底层 socket destroy
 * 失败返回 `GALAY_IO_ERROR`。
 * @note 若 connection 是 `galay_http2_client_conn` 的借用返回值，不应在 client 仍持有时
 * 直接调用本函数，避免 client 后续重复销毁。
 */
galay_status_t galay_http2_conn_destroy(galay_http2_conn_t* conn);

/**
 * @brief 查询连接是否已收到对端 SETTINGS ACK。
 * @param conn connection handle；NULL 返回 `GALAY_FALSE`。
 * @return 已收到返回 `GALAY_TRUE`，否则返回 `GALAY_FALSE`。
 */
galay_bool_t galay_http2_conn_settings_ack_received(const galay_http2_conn_t* conn);

/**
 * @brief 在 connection 上等待并接受一个对端发起的 stream。
 * @param conn server/client connection。
 * @param out_stream 成功时返回 stream 借用指针；归 connection 管理。
 * @param timeout_ms 传递给每次底层 TCP I/O 的 timeout。
 * @return 成功返回 `C_IOResultOk`；超时、协议、GOAWAY/RST、I/O 或状态错误通过
 * `C_IOResult` 和 `value` 返回。
 * @note 该函数会读取并处理控制帧，直到收到新 HEADERS 建立的 stream；等待期间挂起
 * 当前 C coroutine。
 */
C_IOResult galay_http2_conn_accept_stream(galay_http2_conn_t* conn,
                                          galay_http2_stream_t** out_stream,
                                          int64_t timeout_ms);

/**
 * @brief 读取并处理一个 HTTP/2 控制帧。
 * @param conn connection handle。
 * @param timeout_ms 传递给底层 TCP I/O 的 timeout。
 * @return 成功处理 SETTINGS/WINDOW_UPDATE/GOAWAY/RST_STREAM/PING 后返回 `C_IOResultOk`；
 * 错误通过 `C_IOResult` 和 `value` 返回。
 * @note 若读到 DATA/HEADERS，会先处理并继续读取，直到遇到控制帧或出错。
 */
C_IOResult galay_http2_conn_read_control(galay_http2_conn_t* conn, int64_t timeout_ms);

/**
 * @brief 发送 GOAWAY 帧。
 * @param conn connection handle。
 * @param last_stream_id 最后可处理的 stream id；高位会被清除。
 * @param error GOAWAY 中携带的 HTTP/2 错误码。
 * @param timeout_ms 传递给底层 TCP send 的 timeout。
 * @return 成功返回 `C_IOResultOk`；参数、frame size 或 I/O 错误通过 `C_IOResult` 返回。
 * @note 发送 GOAWAY 不会销毁 connection；调用方仍需显式 shutdown/close 外层生命周期。
 */
C_IOResult galay_http2_conn_send_goaway(galay_http2_conn_t* conn,
                                        uint32_t last_stream_id,
                                        galay_http2_error_code_t error,
                                        int64_t timeout_ms);

/**
 * @brief 发送 connection 或 stream WINDOW_UPDATE。
 * @param conn connection handle。
 * @param stream 为 NULL 时发送 connection 级 WINDOW_UPDATE；非 NULL 时也更新该 stream。
 * @param increment window increment，必须在 1 到 2^31 - 1 范围内。
 * @param timeout_ms 传递给底层 TCP send 的 timeout。
 * @return 成功返回 `C_IOResultOk`；参数非法返回 `C_IOResultInvalid`；window 溢出返回
 * `GALAY_HTTP2_ERROR_FLOW_CONTROL`；I/O 错误通过 `C_IOResult` 返回。
 */
C_IOResult galay_http2_conn_send_window_update(galay_http2_conn_t* conn,
                                               galay_http2_stream_t* stream,
                                               uint32_t increment,
                                               int64_t timeout_ms);

/**
 * @brief 销毁单个 stream。
 * @param stream stream handle；不能为 NULL。
 * @return 成功返回 `GALAY_OK`；NULL 返回 `GALAY_INVALID_ARGUMENT`。
 * @note 该函数会从所属 connection 中摘除 stream 并释放未读 pending headers/data；
 * 不会自动发送 RST_STREAM。
 */
galay_status_t galay_http2_stream_destroy(galay_http2_stream_t* stream);

/**
 * @brief 获取 stream id。
 * @param stream stream handle；NULL 时返回 0。
 * @return stream id。
 */
uint32_t galay_http2_stream_id(const galay_http2_stream_t* stream);

/**
 * @brief 获取 stream 当前状态。
 * @param stream stream handle；NULL 时返回 `GALAY_HTTP2_STREAM_CLOSED`。
 * @return 当前 stream 状态。
 */
galay_http2_stream_state_t galay_http2_stream_state(const galay_http2_stream_t* stream);

/**
 * @brief 读取 stream 上的下一组 headers。
 * @param stream stream handle。
 * @param out_headers 成功时返回 headers handle，调用方用 `galay_http2_headers_destroy` 释放。
 * @param timeout_ms 传递给每次底层 TCP I/O 的 timeout。
 * @return 成功返回 `C_IOResultOk`；RST_STREAM、超时、协议或 I/O 错误通过 `C_IOResult`
 * 和 `value` 返回。
 * @note 若当前没有 pending headers，会读取并处理 connection 上的后续帧；等待期间挂起
 * 当前 C coroutine。返回 headers 后所有权从 stream 转移给调用方。
 */
C_IOResult galay_http2_stream_read_headers(galay_http2_stream_t* stream,
                                           galay_http2_headers_t** out_headers,
                                           int64_t timeout_ms);

/**
 * @brief 在 stream 上发送 HEADERS。
 * @param stream stream handle。
 * @param headers 要发送的 headers；调用期间借用，不接管所有权。
 * @param end_stream 是否设置 END_STREAM 并推进本端 stream 状态。
 * @param timeout_ms 传递给底层 TCP send 的 timeout。
 * @return 成功返回 `C_IOResultOk`；stream 已关闭、header 编码/协议错误、flow-control
 * 或 I/O 错误通过 `C_IOResult` 和 `value` 返回。
 */
C_IOResult galay_http2_stream_write_headers(galay_http2_stream_t* stream,
                                            const galay_http2_headers_t* headers,
                                            galay_bool_t end_stream,
                                            int64_t timeout_ms);

/**
 * @brief 读取 stream 上的下一段 DATA。
 * @param stream stream handle。
 * @param out 调用方提供的数据输出 buffer。
 * @param out_len `out` 容量。
 * @param read_len 成功时写入实际读取字节数；失败时置 0。
 * @param end_stream 成功时写入该 DATA 是否携带 END_STREAM；失败时置 `GALAY_FALSE`。
 * @param timeout_ms 传递给每次底层 TCP I/O 的 timeout。
 * @return 成功返回 `C_IOResultOk`，`bytes` 等于 `read_len`；RST_STREAM、buffer 不足、
 * 超时、协议、flow-control 或 I/O 错误通过 `C_IOResult` 和 `value` 返回。
 * @note `out` 由调用方拥有，函数不会保存该指针；若 buffer 不足，pending DATA 会保留
 * 供后续更大 buffer 再读。
 */
C_IOResult galay_http2_stream_read_data(galay_http2_stream_t* stream,
                                        char* out,
                                        size_t out_len,
                                        size_t* read_len,
                                        galay_bool_t* end_stream,
                                        int64_t timeout_ms);

/**
 * @brief 在 stream 上发送一个 DATA 帧。
 * @param stream stream handle。
 * @param data 待发送 bytes；`data_len` 为 0 时可为 NULL。
 * @param data_len 待发送字节数。
 * @param end_stream 是否设置 END_STREAM 并推进本端 stream 状态。
 * @param timeout_ms 传递给底层 TCP send 的 timeout。
 * @return 成功返回 `C_IOResultOk`，`bytes` 等于 `data_len`；stream 已关闭、flow-control
 * 或 I/O 错误通过 `C_IOResult` 和 `value` 返回。
 * @note `data` 调用期间借用，发送前会复制到 frame payload；同一 stream 的写操作需要串行化。
 */
C_IOResult galay_http2_stream_write_data(galay_http2_stream_t* stream,
                                         const char* data,
                                         size_t data_len,
                                         galay_bool_t end_stream,
                                         int64_t timeout_ms);

/**
 * @brief 发送 RST_STREAM 并将本地 stream 标记为关闭。
 * @param stream stream handle。
 * @param error RST_STREAM payload 中携带的 HTTP/2 错误码。
 * @param timeout_ms 传递给底层 TCP send 的 timeout。
 * @return 成功返回 `C_IOResultOk`；参数非法、重复 reset 已关闭 stream 或 I/O 错误通过
 * `C_IOResult` 和 `value` 返回。
 * @note reset 不销毁 stream handle；调用方仍需 `galay_http2_stream_destroy` 或等待
 * connection destroy 释放。
 */
C_IOResult galay_http2_stream_reset(galay_http2_stream_t* stream,
                                    galay_http2_error_code_t error,
                                    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
