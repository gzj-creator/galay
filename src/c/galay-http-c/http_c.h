#ifndef GALAY_C_HTTP_HTTP_C_H
#define GALAY_C_HTTP_HTTP_C_H

/**
 * @file http_c.h
 * @brief Galay HTTP/1.1 C ABI。
 *
 * @details 该头文件暴露最小 HTTP/1.1 C runtime surface，包含请求/响应对象、
 * header 集合、client/server/session 句柄以及 direct C coroutine I/O。所有
 * opaque handle 都由本 ABI 创建和销毁，调用方不能解引用或自行释放内部对象。
 */

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/common-c/host.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP 请求方法。
 * @note 当前 C ABI 只支持 GET 和 POST；设置或解析其他方法会返回参数或协议错误。
 */
typedef enum galay_http_method_t {
    GALAY_HTTP_METHOD_GET = 0,   ///< HTTP GET。
    GALAY_HTTP_METHOD_POST = 1   ///< HTTP POST。
} galay_http_method_t;

/**
 * @brief HTTP 响应状态码。
 * @note 当前序列化器只接受这里列出的状态码。
 */
typedef enum galay_http_status_code_t {
    GALAY_HTTP_STATUS_OK = 200,                       ///< 200 OK。
    GALAY_HTTP_STATUS_NO_CONTENT = 204,               ///< 204 No Content。
    GALAY_HTTP_STATUS_BAD_REQUEST = 400,              ///< 400 Bad Request。
    GALAY_HTTP_STATUS_NOT_FOUND = 404,                ///< 404 Not Found。
    GALAY_HTTP_STATUS_INTERNAL_SERVER_ERROR = 500     ///< 500 Internal Server Error。
} galay_http_status_code_t;

/**
 * @brief HTTP header 集合 opaque handle。
 * @details 由 `galay_http_headers_create` 创建，`galay_http_headers_destroy` 释放。
 * header 名按大小写不敏感语义查找；重复添加同名 header 会合并为逗号分隔值。
 * @note 该 handle 不提供内部同步；同一对象应在同一线程/协程上下文中串行访问。
 */
typedef struct galay_http_headers_t galay_http_headers_t;

/**
 * @brief HTTP request opaque handle。
 * @details 保存 method、path、header、body 和序列化缓存。解析和 setter 会把输入
 * 数据复制进对象；getter 和 serialize 返回的指针借用对象内部存储。
 * @note 返回的借用指针会在对象 mutation、重新 parse/serialize 或 destroy 后失效。
 */
typedef struct galay_http_request_t galay_http_request_t;

/**
 * @brief HTTP response opaque handle。
 * @details 保存 status、header、body 和序列化缓存。解析和 setter 会把输入数据复制
 * 进对象；getter 和 serialize 返回的指针借用对象内部存储。
 * @note 返回的借用指针会在对象 mutation、重新 parse/serialize 或 destroy 后失效。
 */
typedef struct galay_http_response_t galay_http_response_t;

/**
 * @brief HTTP client opaque handle。
 * @details client 独占一条 TCP session，用于 connect、发送一个或多个请求以及接收响应。
 * @note I/O API 必须在 `galay_coro_spawn` 创建的 C coroutine 内调用，并且同一 client
 * 不应被多个线程或 coroutine 并发操作。
 */
typedef struct galay_http_client_t galay_http_client_t;

/**
 * @brief HTTP server opaque handle。
 * @details server 独占 listener socket 和路由表；`serve_one` 会 accept 一个连接、
 * 读取一个请求、调用匹配路由、发送一个响应并关闭该 session。
 * @note server handle 不提供内部同步；bind/listen/route/accept/stop 应串行调用。
 */
typedef struct galay_http_server_t galay_http_server_t;

/**
 * @brief HTTP accepted session opaque handle。
 * @details 由 `galay_http_server_accept` 返回并拥有 accepted TCP socket；调用方负责
 * 使用 session I/O API 后调用 `galay_http_session_destroy` 释放。
 * @note session I/O 必须在 C coroutine 内调用；同一 session 不应并发读写或关闭。
 */
typedef struct galay_http_session_t galay_http_session_t;

/**
 * @brief C HTTP 路由回调。
 *
 * @details `request` 和 `response` 仅在回调期间有效；回调不得保存这两个指针。
 * 回调通过 `response` 填充响应，并以 `galay_status_t` 显式返回失败原因。
 *
 * @param request 已解析完成的请求，借用自 `serve_one` 内部对象。
 * @param response 待填充的响应，借用自 `serve_one` 内部对象。
 * @param user_data 注册路由时传入的用户指针，原样透传，ABI 不拥有。
 * @return `GALAY_OK` 表示响应已填充；非 OK 会被 `serve_one` 转换为 HTTP 500 路径。
 * @note 回调在 server coroutine 中同步执行，不应阻塞 OS 线程或访问已销毁的 user_data。
 */
typedef galay_status_t (*galay_http_route_callback_t)(
    const galay_http_request_t* request,
    galay_http_response_t* response,
    void* user_data);

/**
 * @brief 将 HTTP C ABI 状态码转换为可读字符串。
 * @param status `galay_status_t` 状态码。
 * @return 静态错误字符串，调用方不得释放或修改。
 */
const char* galay_http_get_error(galay_status_t status);

/**
 * @brief 创建空 HTTP header 集合。
 * @param out 成功时写入新建 handle；失败时保持为 NULL。
 * @return `GALAY_OK` 表示成功；`GALAY_INVALID_ARGUMENT` 表示 out 为空；
 *         `GALAY_OUT_OF_MEMORY` 表示分配失败。
 */
galay_status_t galay_http_headers_create(galay_http_headers_t** out);

/**
 * @brief 销毁 HTTP header 集合。
 * @param headers 可为 NULL；销毁后所有由该集合返回的 header value 借用指针失效。
 * @note 该函数不阻塞、不挂起，也不会释放调用方传入过的字符串。
 */
void galay_http_headers_destroy(galay_http_headers_t* headers);

/**
 * @brief 添加或合并一个 HTTP header。
 * @param headers 由 `galay_http_headers_create` 创建的 header 集合。
 * @param name header 名，不能为空字符串；函数会复制并归一化为大小写不敏感 key。
 * @param value header 值，函数会复制该字符串。
 * @return `GALAY_OK` 表示成功；无效指针或空 name 返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_http_headers_add(galay_http_headers_t* headers, const char* name,
                                      const char* value);

/**
 * @brief 按名称查找 HTTP header。
 * @param headers header 集合。
 * @param name 待查找 header 名，大小写不敏感。
 * @param value 成功时写入借用的 value 指针；失败时写入 NULL。
 * @param value_len 成功时写入 value 字节长度；失败时写入 0。
 * @return `GALAY_OK` 表示找到；`GALAY_NOT_FOUND` 表示不存在；参数错误返回
 *         `GALAY_INVALID_ARGUMENT`。
 * @note `value` 指针由 headers 拥有，仅在 headers 未修改且未销毁前有效。
 */
galay_status_t galay_http_headers_find(const galay_http_headers_t* headers, const char* name,
                                       const char** value, size_t* value_len);

/**
 * @brief 删除指定 HTTP header。
 * @param headers header 集合。
 * @param name 待删除 header 名，大小写不敏感。
 * @return `GALAY_OK` 表示已删除；`GALAY_NOT_FOUND` 表示不存在；参数错误返回
 *         `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_http_headers_remove(galay_http_headers_t* headers, const char* name);

/**
 * @brief 创建 HTTP request 对象。
 * @param out 成功时写入 request handle；失败时保持为 NULL。
 * @return `GALAY_OK` 表示成功；参数错误或内存不足通过 `galay_status_t` 返回。
 */
galay_status_t galay_http_request_create(galay_http_request_t** out);

/**
 * @brief 销毁 HTTP request 对象。
 * @param request 可为 NULL；销毁后 path、body、header、serialized 借用指针全部失效。
 */
void galay_http_request_destroy(galay_http_request_t* request);

/**
 * @brief 设置 request method 和 path。
 * @param request request handle。
 * @param method HTTP method，必须是当前枚举支持的值。
 * @param path 请求 path，不能为空字符串；函数会复制该字符串。
 * @return `GALAY_OK` 表示成功；无效 method、空 path 或空 handle 返回
 *         `GALAY_INVALID_ARGUMENT`。
 * @note 调用成功后 request complete 状态会被清除，旧 serialized 借用视图可能失效。
 */
galay_status_t galay_http_request_set_method_path(galay_http_request_t* request,
                                                  galay_http_method_t method,
                                                  const char* path);

/**
 * @brief 读取 request method。
 * @param request request handle。
 * @param method 成功时写入 method。
 * @return `GALAY_OK` 表示成功；空指针返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_http_request_method(const galay_http_request_t* request,
                                         galay_http_method_t* method);

/**
 * @brief 读取 request path。
 * @param request request handle。
 * @param path 成功时写入借用 path 指针；失败时写入 NULL。
 * @param path_len 成功时写入 path 字节长度；失败时写入 0。
 * @return `GALAY_OK` 表示成功；空指针返回 `GALAY_INVALID_ARGUMENT`。
 * @note `path` 由 request 拥有，仅在 request 未修改且未销毁前有效。
 */
galay_status_t galay_http_request_path(const galay_http_request_t* request,
                                       const char** path, size_t* path_len);

/**
 * @brief 向 request 添加 header。
 * @param request request handle。
 * @param name header 名，不能为空字符串。
 * @param value header 值。
 * @return `GALAY_OK` 表示成功；参数错误通过 `galay_status_t` 返回。
 */
galay_status_t galay_http_request_add_header(galay_http_request_t* request, const char* name,
                                             const char* value);

/**
 * @brief 设置 request body。
 * @param request request handle。
 * @param body body 字节指针；`body_len` 为 0 时可为 NULL。
 * @param body_len body 字节数。
 * @return `GALAY_OK` 表示成功；参数错误通过 `galay_status_t` 返回。
 * @note body 会被复制进 request；调用成功后 request complete 状态会被清除。
 */
galay_status_t galay_http_request_set_body(galay_http_request_t* request, const char* body,
                                           size_t body_len);

/**
 * @brief 将 request 序列化为 HTTP/1.1 字节。
 * @param request request handle。
 * @param data 成功时写入借用 serialized buffer；失败时写入 NULL。
 * @param data_len 成功时写入 serialized 字节数；失败时写入 0。
 * @return `GALAY_OK` 表示成功；无效 method/path 或参数错误通过状态码返回。
 * @note 返回 buffer 由 request 拥有，会在 request 修改、再次 serialize 或 destroy 后失效。
 */
galay_status_t galay_http_request_serialize(galay_http_request_t* request,
                                            const char** data, size_t* data_len);

/**
 * @brief 从 HTTP/1.x 字节解析 request。
 * @param request 待写入的 request handle。
 * @param data 输入字节，函数只在调用期间借用并会复制解析结果。
 * @param data_len 输入字节数。
 * @param max_header_len 允许的最大 header 字节数，必须大于 0。
 * @param max_body_len 允许的最大 body 字节数。
 * @param consumed 成功时写入已消费字节数；失败时写入 0。
 * @return `GALAY_OK` 表示解析完整请求；参数错误返回 `GALAY_INVALID_ARGUMENT`；
 *         不完整、超限或协议格式错误返回 `GALAY_PROTOCOL_ERROR`。
 */
galay_status_t galay_http_request_parse(galay_http_request_t* request, const char* data,
                                        size_t data_len, size_t max_header_len,
                                        size_t max_body_len, size_t* consumed);

/**
 * @brief 查询 request 是否为完整解析状态。
 * @param request request handle，可为 NULL。
 * @return `GALAY_TRUE` 表示 request 非空且已完整解析；否则返回 `GALAY_FALSE`。
 */
galay_bool_t galay_http_request_is_complete(const galay_http_request_t* request);

/**
 * @brief 读取 request body。
 * @param request request handle。
 * @param body 成功时写入借用 body 指针；失败时写入 NULL。
 * @param body_len 成功时写入 body 字节数；失败时写入 0。
 * @return `GALAY_OK` 表示成功；空指针返回 `GALAY_INVALID_ARGUMENT`。
 * @note `body` 由 request 拥有，仅在 request 未修改且未销毁前有效。
 */
galay_status_t galay_http_request_body(const galay_http_request_t* request, const char** body,
                                       size_t* body_len);

/**
 * @brief 在 request header 中查找指定名称。
 * @param request request handle。
 * @param name header 名，大小写不敏感。
 * @param value 成功时写入借用 value 指针；失败时写入 NULL。
 * @param value_len 成功时写入 value 字节数；失败时写入 0。
 * @return `GALAY_OK` 表示找到；`GALAY_NOT_FOUND` 表示不存在；参数错误返回
 *         `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_http_request_find_header(const galay_http_request_t* request,
                                              const char* name, const char** value,
                                              size_t* value_len);

/**
 * @brief 创建 HTTP response 对象。
 * @param out 成功时写入 response handle；失败时保持为 NULL。
 * @return `GALAY_OK` 表示成功；参数错误或内存不足通过 `galay_status_t` 返回。
 */
galay_status_t galay_http_response_create(galay_http_response_t** out);

/**
 * @brief 销毁 HTTP response 对象。
 * @param response 可为 NULL；销毁后 body、header、serialized 借用指针全部失效。
 */
void galay_http_response_destroy(galay_http_response_t* response);

/**
 * @brief 设置 response status。
 * @param response response handle。
 * @param status 必须是 `galay_http_status_code_t` 中支持的状态码。
 * @return `GALAY_OK` 表示成功；空 handle 或不支持的状态码返回
 *         `GALAY_INVALID_ARGUMENT`。
 * @note 调用成功后 response complete 状态会被清除，旧 serialized 借用视图可能失效。
 */
galay_status_t galay_http_response_set_status(galay_http_response_t* response,
                                              galay_http_status_code_t status);

/**
 * @brief 读取 response status。
 * @param response response handle。
 * @param status 成功时写入状态码。
 * @return `GALAY_OK` 表示成功；空指针返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_http_response_status(const galay_http_response_t* response,
                                          galay_http_status_code_t* status);

/**
 * @brief 向 response 添加 header。
 * @param response response handle。
 * @param name header 名，不能为空字符串。
 * @param value header 值。
 * @return `GALAY_OK` 表示成功；参数错误通过 `galay_status_t` 返回。
 */
galay_status_t galay_http_response_add_header(galay_http_response_t* response, const char* name,
                                              const char* value);

/**
 * @brief 设置 response body。
 * @param response response handle。
 * @param body body 字节指针；`body_len` 为 0 时可为 NULL。
 * @param body_len body 字节数。
 * @return `GALAY_OK` 表示成功；参数错误通过 `galay_status_t` 返回。
 * @note body 会被复制进 response；调用成功后 response complete 状态会被清除。
 */
galay_status_t galay_http_response_set_body(galay_http_response_t* response, const char* body,
                                            size_t body_len);

/**
 * @brief 读取 response body。
 * @param response response handle。
 * @param body 成功时写入借用 body 指针；失败时写入 NULL。
 * @param body_len 成功时写入 body 字节数；失败时写入 0。
 * @return `GALAY_OK` 表示成功；空指针返回 `GALAY_INVALID_ARGUMENT`。
 * @note `body` 由 response 拥有，仅在 response 未修改且未销毁前有效。
 */
galay_status_t galay_http_response_body(const galay_http_response_t* response, const char** body,
                                        size_t* body_len);

/**
 * @brief 从 HTTP/1.x 字节解析 response。
 * @param response 待写入的 response handle。
 * @param data 输入字节，函数只在调用期间借用并会复制解析结果。
 * @param data_len 输入字节数。
 * @param max_header_len 允许的最大 header 字节数，必须大于 0。
 * @param max_body_len 允许的最大 body 字节数。
 * @param consumed 成功时写入已消费字节数；失败时写入 0。
 * @return `GALAY_OK` 表示解析完整响应；参数错误返回 `GALAY_INVALID_ARGUMENT`；
 *         不完整、超限或协议格式错误返回 `GALAY_PROTOCOL_ERROR`。
 */
galay_status_t galay_http_response_parse(galay_http_response_t* response, const char* data,
                                         size_t data_len, size_t max_header_len,
                                         size_t max_body_len, size_t* consumed);

/**
 * @brief 将 response 序列化为 HTTP/1.1 字节。
 * @param response response handle。
 * @param data 成功时写入借用 serialized buffer；失败时写入 NULL。
 * @param data_len 成功时写入 serialized 字节数；失败时写入 0。
 * @return `GALAY_OK` 表示成功；不支持的 status 或参数错误通过状态码返回。
 * @note 返回 buffer 由 response 拥有，会在 response 修改、再次 serialize 或 destroy 后失效。
 */
galay_status_t galay_http_response_serialize(galay_http_response_t* response,
                                             const char** data, size_t* data_len);

/**
 * @brief 创建 HTTP client 句柄。
 * @param out 成功时写入 client handle；失败时保持为 NULL。
 * @return `GALAY_OK` 表示成功；参数错误或内存不足通过 `galay_status_t` 返回。
 * @note 创建本身不连接网络、不阻塞、不挂起。
 */
galay_status_t galay_http_client_create(galay_http_client_t** out);

/**
 * @brief 销毁 HTTP client 句柄及其内部 socket。
 * @param client client handle。
 * @return `GALAY_OK` 表示释放成功；空 handle 返回 `GALAY_INVALID_ARGUMENT`；
 *         底层 socket destroy 失败返回 `GALAY_IO_ERROR`。
 * @note destroy 不会执行 coroutine close；需要优雅关闭时先调用 `galay_http_client_close`。
 */
galay_status_t galay_http_client_destroy(galay_http_client_t* client);

/**
 * @brief 在当前 C coroutine 内连接 HTTP server endpoint。
 * @param client client handle。
 * @param endpoint 目标地址；只在调用期间借用。
 * @param timeout_ms 负数无限等待，0 立即超时，正数为毫秒超时。
 * @return `C_IOResultOk` 表示连接成功；超时、取消、参数错误、socket 创建或网络错误通过
 *         `C_IOResult.code`/`sys_errno` 显式返回。
 * @note 该函数会挂起当前 C coroutine，不阻塞 OS 线程；失败时会销毁已创建的 socket。
 */
C_IOResult galay_http_client_connect(galay_http_client_t* client, const C_Host* endpoint,
                                     int64_t timeout_ms);

/**
 * @brief 发送一个完整 HTTP request。
 * @param client 已连接的 client handle。
 * @param request request handle；调用返回前必须保持有效。
 * @param timeout_ms 每次 socket send 的毫秒超时语义。
 * @return `C_IOResultOk` 表示完整 request 已发送，`bytes` 为发送字节数；序列化失败、
 *         EOF、超时、取消或网络错误通过 `C_IOResult` 返回。
 * @note 函数会先序列化 request，并在 socket I/O 期间挂起当前 C coroutine。
 */
C_IOResult galay_http_client_send_request(galay_http_client_t* client,
                                          const galay_http_request_t* request,
                                          int64_t timeout_ms);

/**
 * @brief 接收并解析一个完整 HTTP response。
 * @param client 已连接的 client handle。
 * @param out_response 成功时写入新分配 response；失败时写入 NULL。
 * @param max_header_len 最大 header 字节数，必须大于 0。
 * @param max_body_len 最大 body 字节数。
 * @param timeout_ms 每次 socket recv 的毫秒超时语义。
 * @return `C_IOResultOk` 表示成功，`ptr` 指向同一个 response，`bytes` 为消费字节数；
 *         EOF、超时、取消、协议错误或网络错误通过 `C_IOResult` 返回。
 * @note 调用方必须用 `galay_http_response_destroy` 释放成功返回的 response。
 */
C_IOResult galay_http_client_recv_response(galay_http_client_t* client,
                                           galay_http_response_t** out_response,
                                           size_t max_header_len,
                                           size_t max_body_len,
                                           int64_t timeout_ms);

/**
 * @brief 关闭 HTTP client 底层 TCP session。
 * @param client client handle。
 * @param timeout_ms close 操作的毫秒超时语义。
 * @return `C_IOResultOk` 表示 close 成功；空 handle、已关闭状态、超时、取消或 I/O
 *         错误通过 `C_IOResult` 返回。
 * @note close 不销毁 client；调用方仍需调用 `galay_http_client_destroy`。
 */
C_IOResult galay_http_client_close(galay_http_client_t* client, int64_t timeout_ms);

/**
 * @brief 创建 HTTP server 句柄。
 * @param out 成功时写入 server handle；失败时保持为 NULL。
 * @return `GALAY_OK` 表示成功；参数错误或内存不足通过 `galay_status_t` 返回。
 */
galay_status_t galay_http_server_create(galay_http_server_t** out);

/**
 * @brief 销毁 HTTP server 句柄及其 listener socket。
 * @param server server handle。
 * @return `GALAY_OK` 表示释放成功；空 handle 返回 `GALAY_INVALID_ARGUMENT`；
 *         底层 socket destroy 失败返回 `GALAY_IO_ERROR`。
 * @note 需要停止监听时先调用 `galay_http_server_stop`，再 destroy。
 */
galay_status_t galay_http_server_destroy(galay_http_server_t* server);

/**
 * @brief 绑定 HTTP server listener 地址。
 * @param server server handle。
 * @param endpoint 本地监听地址；只在调用期间借用。
 * @return `GALAY_OK` 表示 bind 成功；参数错误、socket 创建失败或底层 bind 失败通过
 *         `galay_status_t` 返回。
 * @note 该函数是同步生命周期操作，不挂起 C coroutine。
 */
galay_status_t galay_http_server_bind(galay_http_server_t* server, const C_Host* endpoint);

/**
 * @brief 开始监听 HTTP server listener。
 * @param server 已 bind 的 server handle。
 * @param backlog listen backlog，必须大于 0。
 * @return `GALAY_OK` 表示 listen 成功；无效状态或底层 I/O 失败通过状态码返回。
 */
galay_status_t galay_http_server_listen(galay_http_server_t* server, int backlog);

/**
 * @brief 查询 server listener 实际本地地址。
 * @param server 已 bind 的 server handle。
 * @param endpoint 成功时写入本地地址。
 * @return `GALAY_OK` 表示成功；无效状态或底层查询失败通过状态码返回。
 * @note 常用于端口传 0 后读取系统分配端口。
 */
galay_status_t galay_http_server_local_endpoint(const galay_http_server_t* server,
                                                C_Host* endpoint);

/**
 * @brief 注册或替换精确 method/path 路由。
 * @param server server handle。
 * @param method HTTP method。
 * @param path 精确匹配 path，不能为空；函数会复制该字符串。
 * @param callback 路由回调，不能为空。
 * @param user_data 用户上下文指针，原样保存，ABI 不拥有。
 * @return `GALAY_OK` 表示注册成功；无效参数通过状态码返回。
 * @note 同 method/path 重复注册会替换 callback 和 user_data。
 */
galay_status_t galay_http_server_add_route(galay_http_server_t* server,
                                           galay_http_method_t method,
                                           const char* path,
                                           galay_http_route_callback_t callback,
                                           void* user_data);

/**
 * @brief 在当前 C coroutine 内 accept 一个 HTTP TCP session。
 * @param server 已 listen 的 server handle。
 * @param out_session 成功时写入新分配 session；失败时写入 NULL。
 * @param out_peer 可选输出 peer 地址；不需要时传 NULL。
 * @param timeout_ms accept 操作的毫秒超时语义。
 * @return `C_IOResultOk` 表示成功，`ptr` 指向同一个 session；超时、取消、参数错误或
 *         I/O 错误通过 `C_IOResult` 返回。
 * @note 调用方必须用 `galay_http_session_destroy` 释放成功返回的 session。
 */
C_IOResult galay_http_server_accept(galay_http_server_t* server,
                                    galay_http_session_t** out_session,
                                    C_Host* out_peer,
                                    int64_t timeout_ms);

/**
 * @brief accept、处理并关闭一个 HTTP 请求。
 * @param server 已 listen 且已注册路由的 server handle。
 * @param timeout_ms accept/read/write/close 操作共用的毫秒超时语义。
 * @return `C_IOResultOk` 表示一次请求响应闭环完成；accept、recv、callback、
 *         send、close 或销毁失败通过 `C_IOResult` 返回。
 * @details 未匹配路由会自动返回 404；callback 返回非 OK 时会尽量返回 500。
 * @note 该函数会挂起当前 C coroutine，并在返回前释放本次 request/response/session。
 */
C_IOResult galay_http_server_serve_one(galay_http_server_t* server, int64_t timeout_ms);

/**
 * @brief 关闭 HTTP server listener 并停止监听。
 * @param server 正在 listening 的 server handle。
 * @param timeout_ms close 操作的毫秒超时语义。
 * @return `C_IOResultOk` 表示 listener 已关闭；无效状态、超时、取消或 I/O 错误通过
 *         `C_IOResult` 返回。
 * @note stop 不销毁 server 或路由表；调用方仍需调用 `galay_http_server_destroy`。
 */
C_IOResult galay_http_server_stop(galay_http_server_t* server, int64_t timeout_ms);

/**
 * @brief 销毁 HTTP session 及其 accepted socket。
 * @param session session handle。
 * @return `GALAY_OK` 表示释放成功；空 handle 返回 `GALAY_INVALID_ARGUMENT`；
 *         底层 socket destroy 失败返回 `GALAY_IO_ERROR`。
 * @note 需要优雅关闭时先调用 `galay_http_session_close`，再 destroy。
 */
galay_status_t galay_http_session_destroy(galay_http_session_t* session);

/**
 * @brief 通过 session 发送一个完整 HTTP request。
 * @param session 已连接 session。
 * @param request request handle；调用返回前必须保持有效。
 * @param timeout_ms 每次 socket send 的毫秒超时语义。
 * @return `C_IOResultOk` 表示完整 request 已发送，`bytes` 为发送字节数；错误通过
 *         `C_IOResult` 返回。
 * @note 该函数会挂起当前 C coroutine；不会保存 request 指针。
 */
C_IOResult galay_http_session_send_request(galay_http_session_t* session,
                                           const galay_http_request_t* request,
                                           int64_t timeout_ms);

/**
 * @brief 通过 session 接收并解析一个完整 HTTP request。
 * @param session 已连接 session。
 * @param out_request 成功时写入新分配 request；失败时写入 NULL。
 * @param max_header_len 最大 header 字节数，必须大于 0。
 * @param max_body_len 最大 body 字节数。
 * @param timeout_ms 每次 socket recv 的毫秒超时语义。
 * @return `C_IOResultOk` 表示成功，`ptr` 指向同一个 request，`bytes` 为消费字节数；
 *         EOF、超时、取消、协议错误或网络错误通过 `C_IOResult` 返回。
 * @note 调用方必须用 `galay_http_request_destroy` 释放成功返回的 request。
 */
C_IOResult galay_http_session_recv_request(galay_http_session_t* session,
                                           galay_http_request_t** out_request,
                                           size_t max_header_len,
                                           size_t max_body_len,
                                           int64_t timeout_ms);

/**
 * @brief 通过 session 发送一个完整 HTTP response。
 * @param session 已连接 session。
 * @param response response handle；调用返回前必须保持有效。
 * @param timeout_ms 每次 socket send 的毫秒超时语义。
 * @return `C_IOResultOk` 表示完整 response 已发送，`bytes` 为发送字节数；错误通过
 *         `C_IOResult` 返回。
 * @note 该函数会挂起当前 C coroutine；不会保存 response 指针。
 */
C_IOResult galay_http_session_send_response(galay_http_session_t* session,
                                            const galay_http_response_t* response,
                                            int64_t timeout_ms);

/**
 * @brief 通过 session 接收并解析一个完整 HTTP response。
 * @param session 已连接 session。
 * @param out_response 成功时写入新分配 response；失败时写入 NULL。
 * @param max_header_len 最大 header 字节数，必须大于 0。
 * @param max_body_len 最大 body 字节数。
 * @param timeout_ms 每次 socket recv 的毫秒超时语义。
 * @return `C_IOResultOk` 表示成功，`ptr` 指向同一个 response，`bytes` 为消费字节数；
 *         EOF、超时、取消、协议错误或网络错误通过 `C_IOResult` 返回。
 * @note 调用方必须用 `galay_http_response_destroy` 释放成功返回的 response。
 */
C_IOResult galay_http_session_recv_response(galay_http_session_t* session,
                                            galay_http_response_t** out_response,
                                            size_t max_header_len,
                                            size_t max_body_len,
                                            int64_t timeout_ms);

/**
 * @brief 通过 session 发送原始字节。
 * @param session 已连接 session。
 * @param data 待发送字节；`data_len` 为 0 时可为 NULL。
 * @param data_len 待发送字节数。
 * @param timeout_ms 每次 socket send 的毫秒超时语义。
 * @return `C_IOResultOk` 表示所有字节已发送，`bytes` 为发送字节数；错误通过
 *         `C_IOResult` 返回。
 * @note 该 streaming surface 用于分片、超时、关闭和协议错误测试；不会保存 data。
 */
C_IOResult galay_http_session_send_bytes(galay_http_session_t* session, const char* data,
                                         size_t data_len, int64_t timeout_ms);

/**
 * @brief 通过 session 接收原始字节。
 * @param session 已连接 session。
 * @param data 调用方提供的输出缓冲区，不能为空。
 * @param data_len 输出缓冲区长度，必须大于 0。
 * @param timeout_ms socket recv 的毫秒超时语义。
 * @return `C_IOResultOk` 表示读到字节，`bytes` 为实际读取数；peer 关闭且读到 0 字节返回
 *         `C_IOResultEof`；其他错误通过 `C_IOResult` 返回。
 * @note 该函数只写入调用方缓冲区，不保存 data 指针。
 */
C_IOResult galay_http_session_recv_bytes(galay_http_session_t* session, char* data,
                                         size_t data_len, int64_t timeout_ms);

/**
 * @brief 关闭 HTTP session 底层 TCP 连接。
 * @param session session handle。
 * @param timeout_ms close 操作的毫秒超时语义。
 * @return `C_IOResultOk` 表示 close 成功；空 handle、已关闭状态、超时、取消或 I/O
 *         错误通过 `C_IOResult` 返回。
 * @note close 不销毁 session；调用方仍需调用 `galay_http_session_destroy`。
 */
C_IOResult galay_http_session_close(galay_http_session_t* session, int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
