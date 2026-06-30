/**
 * @file ssl.h
 * @brief TLS/SSL C ABI。
 * @details
 * 该头文件暴露基于 OpenSSL 的 TLS context 和 TLS socket C 接口。context 负责证书、
 * CA、校验模式、ALPN 和 session cache 配置；socket 持有底层 TCP transport 与 SSL
 * engine。所有成功创建的 context/socket 都由调用方拥有，并必须通过对应 destroy
 * 函数释放。
 *
 * 返回 `galay_status_t` 的配置接口会把参数错误、文件不存在和 OpenSSL 加载失败映射为
 * 公开错误码。返回 `C_IOResult` 的 I/O 接口使用 `code` 表示通用 I/O 状态，`bytes`
 * 表示本次成功传输的明文或密文相关字节数；SSL 层错误不会在 `value` 中暴露额外业务码。
 *
 * @note 本 C ABI 不提供内部同步。context 配置应在创建 socket 前完成；同一 socket 的
 * connect/handshake/recv/send/shutdown/close 不能被多个线程或协程并发调用。I/O 函数
 * 通过底层 TCP C ABI 等待网络事件，等待期间挂起当前 C coroutine；不应在未运行 galay
 * C runtime 的上下文中调用需要网络等待的函数。
 */
#ifndef GALAY_C_SSL_SSL_H
#define GALAY_C_SSL_SSL_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/common-c/host.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TLS context 方法。
 * @details client context 用于主动 connect/handshake；server context 用于 listen/accept
 * 后的握手。
 */
typedef enum galay_ssl_method_t {
    GALAY_SSL_METHOD_TLS_CLIENT = 0, ///< TLS client 模式。
    GALAY_SSL_METHOD_TLS_SERVER = 1  ///< TLS server 模式。
} galay_ssl_method_t;

/**
 * @brief 证书校验模式。
 * @details 设置到 context 后影响之后基于该 context 创建或握手的 socket。
 */
typedef enum galay_ssl_verify_mode_t {
    GALAY_SSL_VERIFY_NONE = 0, ///< 不校验对端证书。
    GALAY_SSL_VERIFY_PEER = 1  ///< 校验对端证书。
} galay_ssl_verify_mode_t;

/**
 * @brief OpenSSL session cache 模式。
 * @details 枚举值会映射到 OpenSSL `SSL_SESS_CACHE_*` 标志。
 */
typedef enum galay_ssl_session_cache_mode_t {
    GALAY_SSL_SESSION_CACHE_OFF = 0,    ///< 关闭 session cache。
    GALAY_SSL_SESSION_CACHE_CLIENT = 1, ///< 启用 client session cache。
    GALAY_SSL_SESSION_CACHE_SERVER = 2, ///< 启用 server session cache。
    GALAY_SSL_SESSION_CACHE_BOTH = 3    ///< 同时启用 client/server session cache。
} galay_ssl_session_cache_mode_t;

/**
 * @brief TLS context handle。
 * @details context 拥有 OpenSSL SSL_CTX。socket 只借用 context 指针，因此 context 必须
 * 活到所有基于它创建/accept 的 socket destroy 之后。
 */
typedef struct galay_ssl_context_t galay_ssl_context_t;

/**
 * @brief TLS socket handle。
 * @details socket 拥有底层 TCP socket 和 SSL engine；destroy 会释放两者。close 只关闭
 * 底层 TCP 连接，不释放 handle。
 */
typedef struct galay_ssl_socket_t galay_ssl_socket_t;

/**
 * @brief 将通用 `galay_status_t` 转为可读字符串。
 * @param status 任意 galay C status。
 * @return 静态字符串；调用方不得释放。
 */
const char* galay_ssl_get_error(galay_status_t status);

/**
 * @brief 创建 TLS context。
 * @param method TLS client/server 方法。
 * @param out 成功时返回 context，调用方用 `galay_ssl_context_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；method/out 无效返回 `GALAY_INVALID_ARGUMENT`；
 * 分配失败返回 `GALAY_OUT_OF_MEMORY`；OpenSSL context 初始化失败返回 `GALAY_IO_ERROR`。
 */
galay_status_t galay_ssl_context_create(galay_ssl_method_t method, galay_ssl_context_t** out);

/**
 * @brief 销毁 TLS context。
 * @param context 可为 NULL；非 NULL 时必须确保没有 socket 仍借用该 context。
 */
void galay_ssl_context_destroy(galay_ssl_context_t* context);

/**
 * @brief 从文件加载证书链。
 * @param context TLS context。
 * @param path 证书文件路径；调用期间借用，不会被保存。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；文件不存在返回
 * `GALAY_NOT_FOUND`；文件检查或 OpenSSL 加载失败返回 `GALAY_IO_ERROR`。
 * @note 加载结果保存在 context/OpenSSL 内部，调用返回后 path buffer 可释放或修改。
 */
galay_status_t galay_ssl_context_load_certificate(galay_ssl_context_t* context, const char* path);

/**
 * @brief 从文件加载私钥。
 * @param context TLS context。
 * @param path 私钥文件路径；调用期间借用，不会被保存。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；文件不存在返回
 * `GALAY_NOT_FOUND`；文件检查或 OpenSSL 加载失败返回 `GALAY_IO_ERROR`。
 */
galay_status_t galay_ssl_context_load_private_key(galay_ssl_context_t* context, const char* path);

/**
 * @brief 从文件加载 CA 证书。
 * @param context TLS context。
 * @param path CA 文件路径；调用期间借用，不会被保存。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；文件不存在返回
 * `GALAY_NOT_FOUND`；文件检查或 OpenSSL 加载失败返回 `GALAY_IO_ERROR`。
 */
galay_status_t galay_ssl_context_load_ca(galay_ssl_context_t* context, const char* path);

/**
 * @brief 设置证书校验模式。
 * @param context TLS context。
 * @param mode 校验模式。
 * @return 成功返回 `GALAY_OK`；context 或 mode 无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note 应在创建 socket 或执行握手前完成配置；本函数不提供并发配置同步。
 */
galay_status_t galay_ssl_context_set_verify_mode(galay_ssl_context_t* context,
                                                 galay_ssl_verify_mode_t mode);

/**
 * @brief 设置客户端 ALPN offer 列表。
 * @details 列表会转换为 OpenSSL ALPN wire format，用于 client handshake。
 * @param context TLS client context。
 * @param protocols 协议字符串数组，例如 "h2"、"http/1.1"；数组和元素仅在调用期间借用。
 * @param count 协议数量，必须大于 0。
 * @return 成功返回 `GALAY_OK`；参数、协议空串或协议长度超过 255 字节返回
 * `GALAY_INVALID_ARGUMENT`；OpenSSL 配置失败返回 `GALAY_IO_ERROR`。
 * @note protocols/count 仅在调用期间借用；每个协议必须非空且长度不超过 255 字节。
 */
galay_status_t galay_ssl_context_set_alpn_protocols(galay_ssl_context_t* context,
                                                    const char* const* protocols,
                                                    size_t count);

/**
 * @brief 设置服务端 ALPN 选择优先级。
 * @details 服务端握手时会按列表顺序选择与 client offer 匹配的协议。
 * @param context TLS server context。
 * @param protocols 协议字符串数组；数组和元素仅在调用期间借用。
 * @param count 协议数量，必须大于 0。
 * @return 成功返回 `GALAY_OK`；参数、协议空串或协议长度超过 255 字节返回
 * `GALAY_INVALID_ARGUMENT`；OpenSSL 配置失败返回 `GALAY_IO_ERROR`。
 * @note protocols/count 仅在调用期间借用；列表顺序表示服务端选择优先级。
 */
galay_status_t galay_ssl_context_set_alpn_select_protocols(galay_ssl_context_t* context,
                                                           const char* const* protocols,
                                                           size_t count);

/**
 * @brief 设置 OpenSSL session cache 模式。
 * @param context TLS context。
 * @param mode session cache 模式。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`，成功返回 `GALAY_OK`。
 * @note 应在握手前设置；本函数只配置 context，不影响已经完成握手的 socket。
 */
galay_status_t galay_ssl_context_set_session_cache_mode(galay_ssl_context_t* context,
                                                        galay_ssl_session_cache_mode_t mode);

/**
 * @brief 设置 session 超时时间，单位为秒。
 * @param context TLS context。
 * @param timeout_seconds session timeout 秒数，必须大于等于 0。
 * @return 成功返回 `GALAY_OK`；context 无效或 timeout 为负返回 `GALAY_INVALID_ARGUMENT`。
 * @note timeout_seconds 必须大于等于 0；具体 session 过期行为由 OpenSSL 管理。
 */
galay_status_t galay_ssl_context_set_session_timeout(galay_ssl_context_t* context,
                                                     long timeout_seconds);

/**
 * @brief 禁用 context 的 session cache。
 * @param context TLS context。
 * @return 成功返回 `GALAY_OK`；context 无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_ssl_context_disable_session_cache(galay_ssl_context_t* context);

/**
 * @brief 禁用 context 的 TLS session ticket。
 * @param context TLS context。
 * @return 成功返回 `GALAY_OK`；context 无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_ssl_context_disable_session_tickets(galay_ssl_context_t* context);

/**
 * @brief 创建 TLS socket。
 * @details 创建底层 TCP socket 并初始化 SSL engine。socket 只借用 context，context 必须
 * 比 socket 活得更久。
 * @param context TLS context。
 * @param type IPv4 或 IPv6。
 * @param out 成功时返回 socket，调用方用 `galay_ssl_socket_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；分配失败返回
 * `GALAY_OUT_OF_MEMORY`；TCP 或 SSL engine 初始化失败返回 `GALAY_IO_ERROR`。
 */
galay_status_t galay_ssl_socket_create(galay_ssl_context_t* context, C_IPType type,
                                       galay_ssl_socket_t** out);

/**
 * @brief 销毁 TLS socket。
 * @param socket 可为 NULL；非 NULL 时会释放 SSL engine 并 destroy 底层 TCP socket。
 * @note 该函数不执行 TLS close_notify；需要有序关闭时先调用 `galay_ssl_socket_shutdown`。
 */
void galay_ssl_socket_destroy(galay_ssl_socket_t* socket);

/**
 * @brief 绑定 TLS socket 的本地地址。
 * @param socket server listener socket。
 * @param host 本地地址；调用期间借用。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；底层 bind 失败返回
 * `GALAY_IO_ERROR`。
 */
galay_status_t galay_ssl_socket_bind(galay_ssl_socket_t* socket, const C_Host* host);

/**
 * @brief 将 TLS socket 作为 TCP listener 开始监听。
 * @param socket 已 bind 的 server socket。
 * @param backlog listen backlog。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；底层 listen 失败返回
 * `GALAY_IO_ERROR`。
 */
galay_status_t galay_ssl_socket_listen(galay_ssl_socket_t* socket, int backlog);

/**
 * @brief 获取 socket 当前本地 endpoint。
 * @param socket TLS socket。
 * @param out 成功时写入 endpoint。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；底层查询失败返回
 * `GALAY_IO_ERROR`。
 */
galay_status_t galay_ssl_socket_local_endpoint(const galay_ssl_socket_t* socket, C_Host* out);

/**
 * @brief 设置客户端 SNI hostname。
 * @param socket client TLS socket；必须已创建 SSL engine 且尚未开始握手。
 * @param hostname 非空主机名；调用期间借用。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`；OpenSSL 设置失败返回
 * `GALAY_IO_ERROR`。
 * @note hostname 指针不会被 C ABI 保存；应在 connect/handshake 前调用。
 */
galay_status_t galay_ssl_socket_set_hostname(galay_ssl_socket_t* socket, const char* hostname);

/**
 * @brief 在 listener 上接受一个 TCP 连接并创建 TLS socket。
 * @param listener 已 listen 的 server socket。
 * @param out 成功时返回 accepted socket；调用前 `*out` 必须为 NULL，调用方负责 destroy。
 * @param out_peer 可为 NULL；非 NULL 时写入 peer endpoint。
 * @param timeout_ms 传递给底层 TCP accept 的 timeout。
 * @return 成功返回 `C_IOResultOk`，`ptr` 与 `*out` 指向 accepted socket；超时/取消/错误
 * 透传底层 TCP `C_IOResult`；参数无效返回 `C_IOResultInvalid`。
 * @note accepted socket 借用 listener 的 context；该 context 必须在 accepted socket
 * destroy 后才能销毁。accept 只建立 TCP 连接，仍需调用 `galay_ssl_socket_handshake`。
 */
C_IOResult galay_ssl_socket_accept(galay_ssl_socket_t* listener, galay_ssl_socket_t** out,
                                   C_Host* out_peer, int64_t timeout_ms);

/**
 * @brief 连接远端 TCP endpoint。
 * @param socket client TLS socket。
 * @param host 远端地址；调用期间借用。
 * @param timeout_ms 传递给底层 TCP connect 的 timeout。
 * @return 成功返回 `C_IOResultOk`；超时/取消/错误透传底层 TCP `C_IOResult`；参数无效返回
 * `C_IOResultInvalid`。
 * @note connect 只建立 TCP 连接，仍需调用 `galay_ssl_socket_handshake` 完成 TLS 握手。
 */
C_IOResult galay_ssl_socket_connect(galay_ssl_socket_t* socket, const C_Host* host,
                                    int64_t timeout_ms);

/**
 * @brief 驱动 TLS 握手。
 * @param socket 已连接或 accepted 的 TLS socket。
 * @param timeout_ms 传递给每次底层 TCP send/recv 的 timeout；不是整个握手的绝对 deadline。
 * @return 成功返回 `C_IOResultOk`；OpenSSL `SSL_ERROR_ZERO_RETURN` 映射为 `C_IOResultEof`；
 * OpenSSL Error/Syscall 映射为 `C_IOResultError`；底层 TCP 超时/取消/错误按其 `code`
 * 返回。
 * @note 握手可能多次读写密文并挂起当前 C coroutine；重复调用已完成握手的 socket 会直接成功。
 */
C_IOResult galay_ssl_socket_handshake(galay_ssl_socket_t* socket, int64_t timeout_ms);

/**
 * @brief 接收 TLS 明文数据。
 * @param socket 已完成握手的 TLS socket。
 * @param buffer 调用方提供的明文输出 buffer。
 * @param length buffer 容量，必须大于 0。
 * @param timeout_ms 传递给每次底层 TCP send/recv 的 timeout。
 * @return 成功返回 `C_IOResultOk`，`bytes` 为读取明文字节数；TLS close_notify/EOF 返回
 * `C_IOResultEof`；OpenSSL 或底层 TCP 错误通过 `C_IOResult` 返回。
 * @note `buffer` 调用期间借用，函数不会保存；读取过程中可能需要 flush TLS 密文并挂起
 * 当前 C coroutine。
 */
C_IOResult galay_ssl_socket_recv(galay_ssl_socket_t* socket, char* buffer, size_t length,
                                 int64_t timeout_ms);

/**
 * @brief 发送 TLS 明文数据。
 * @param socket 已完成握手的 TLS socket。
 * @param buffer 待发送明文；调用期间借用。
 * @param length 待发送字节数，必须大于 0。
 * @param timeout_ms 传递给每次底层 TCP send/recv 的 timeout。
 * @return 完整发送成功返回 `C_IOResultOk`，`bytes` 为 `length`；若 OpenSSL 或底层 TCP
 * 出错返回对应 `C_IOResult`，部分发送时 `bytes` 可能小于 `length`。
 * @note send 可能因 TLS 状态机需要读取密文或 flush 密文而挂起当前 C coroutine。
 */
C_IOResult galay_ssl_socket_send(galay_ssl_socket_t* socket, const char* buffer, size_t length,
                                 int64_t timeout_ms);

/**
 * @brief 执行 TLS shutdown/close_notify。
 * @param socket TLS socket。
 * @param timeout_ms 传递给每次底层 TCP send/recv 的 timeout。
 * @return close_notify 成功或对端已 EOF 返回 `C_IOResultOk`；参数无效返回
 * `C_IOResultInvalid`；OpenSSL 或底层 TCP 错误通过 `C_IOResult` 返回。
 * @note shutdown 不关闭或销毁底层 TCP socket；需要释放连接时继续调用
 * `galay_ssl_socket_close` 和/或 `galay_ssl_socket_destroy`。
 */
C_IOResult galay_ssl_socket_shutdown(galay_ssl_socket_t* socket, int64_t timeout_ms);

/**
 * @brief 关闭底层 TCP socket。
 * @param socket TLS socket。
 * @param timeout_ms 传递给底层 TCP close 的 timeout。
 * @return 成功返回 `C_IOResultOk`；参数无效返回 `C_IOResultInvalid`；底层 TCP close 的
 * 超时/取消/错误按其 `C_IOResult` 返回。
 * @note close 不销毁 socket handle，也不保证已发送 TLS close_notify；有序 TLS 关闭请先调用
 * `galay_ssl_socket_shutdown`。
 */
C_IOResult galay_ssl_socket_close(galay_ssl_socket_t* socket, int64_t timeout_ms);

/**
 * @brief 获取握手后协商出的 TLS 协议版本字符串。
 * @param socket TLS socket。
 * @param out 调用方输出 buffer。
 * @param out_len `out` 容量。
 * @param written 成功或 buffer 查询时写入协议字符串字节数。
 * @return 成功返回 `GALAY_OK`；socket 无效、out/written 为空或 buffer 不足返回
 * `GALAY_INVALID_ARGUMENT`。
 * @note 写入内容不追加 NUL；`out` 由调用方拥有，函数不会保存。
 */
galay_status_t galay_ssl_socket_get_protocol(const galay_ssl_socket_t* socket, char* out,
                                             size_t out_len, size_t* written);

/**
 * @brief 获取握手后协商出的 cipher suite 字符串。
 * @param socket TLS socket。
 * @param out 调用方输出 buffer。
 * @param out_len `out` 容量。
 * @param written 成功或 buffer 查询时写入 cipher 字符串字节数。
 * @return 成功返回 `GALAY_OK`；socket 无效、out/written 为空或 buffer 不足返回
 * `GALAY_INVALID_ARGUMENT`。
 * @note 写入内容不追加 NUL；`out` 由调用方拥有，函数不会保存。
 */
galay_status_t galay_ssl_socket_get_cipher(const galay_ssl_socket_t* socket, char* out,
                                           size_t out_len, size_t* written);

/**
 * @brief 获取握手后协商出的 ALPN 协议。
 * @param socket TLS socket。
 * @param out 调用方输出 buffer。
 * @param out_len `out` 容量。
 * @param written 成功或 buffer 查询时写入 ALPN 协议字节数。
 * @return 成功返回 `GALAY_OK`；socket 无效、out/written 为空或 buffer 不足返回
 * `GALAY_INVALID_ARGUMENT`。
 * @note out 由调用方拥有；写入内容不追加 NUL，实际字节数通过 written 返回。未协商 ALPN 时
 * written 为 0。
 */
galay_status_t galay_ssl_socket_get_negotiated_alpn(const galay_ssl_socket_t* socket, char* out,
                                                    size_t out_len, size_t* written);

#ifdef __cplusplus
}
#endif

#endif
