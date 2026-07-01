/**
 * @file mcp_c.h
 * @brief MCP C ABI 消息构造、解析、client/server 与工具注册接口。
 * @details 所有 handle 均为不透明 C 对象，通过 create/destroy 管理所有权；
 * JSON buffer 访问器返回借用视图，失败通过 `galay_status_t` 或 `C_IOResult` 显式返回。
 */
#ifndef GALAY_C_MCP_MCP_C_H
#define GALAY_C_MCP_MCP_C_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MCP transport 模式。
 * @details STDIO 当前用于本地 loopback server；HTTP 通过 C coroutine TCP socket
 * 发送 JSON-RPC over HTTP 请求。
 */
typedef enum galay_mcp_mode_t {
    GALAY_MCP_MODE_STDIO = 0, ///< 本地 stdio/loopback 模式。
    GALAY_MCP_MODE_HTTP = 1   ///< HTTP transport 模式。
} galay_mcp_mode_t;

/**
 * @brief MCP JSON message handle。
 * @details 由 create 或 async client 调用创建；调用方拥有并用
 * `galay_mcp_message_destroy` 释放。`galay_mcp_message_data` 返回借用内部 JSON buffer。
 */
typedef struct galay_mcp_message_t galay_mcp_message_t;

/**
 * @brief 已解析 MCP request handle。
 * @details parse 成功后由调用方拥有；method/params accessor 返回借用 buffer。
 */
typedef struct galay_mcp_parsed_request_t galay_mcp_parsed_request_t;

/**
 * @brief 已解析 MCP response handle。
 * @details parse 成功后由调用方拥有；result accessor 返回借用 buffer。
 */
typedef struct galay_mcp_parsed_response_t galay_mcp_parsed_response_t;

/**
 * @brief MCP client 配置 handle。
 * @details 创建时复制 URL/token 等配置，传给 client create 后 client 再复制一份。
 */
typedef struct galay_mcp_client_config_t galay_mcp_client_config_t;

/**
 * @brief MCP client handle。
 * @details HTTP client 调用使用 C coroutine TCP I/O；STDIO client 借用 loopback server。
 * handle 不提供跨线程同步，建议在同一 runtime/coroutine 调度上下文内顺序使用。
 */
typedef struct galay_mcp_client_t galay_mcp_client_t;

/**
 * @brief MCP server handle。
 * @details server 拥有注册表和可选 HTTP listener；destroy 会释放 listener handle，
 * 但不会管理回调 userdata 的生命周期。
 */
typedef struct galay_mcp_server_t galay_mcp_server_t;

/**
 * @brief MCP tool 调用回调。
 * @param arguments 借用的 JSON arguments buffer，仅在回调期间有效。
 * @param arguments_len arguments 字节数，不保证 NUL 结尾。
 * @param result 借用的输出 message；回调应写入 JSON result 对象。
 * @param userdata 注册 tool 时传入的用户指针，生命周期由调用方管理。
 * @return `GALAY_OK` 表示已生成 result；非 OK 会转换为 JSON-RPC internal error 响应。
 * @note 回调在 server 处理请求的 coroutine/调用栈内同步执行；不得保存 arguments/result 指针。
 */
typedef galay_status_t (*galay_mcp_tool_handler_fn)(const char* arguments,
                                                    size_t arguments_len,
                                                    galay_mcp_message_t* result,
                                                    void* userdata);

/**
 * @brief MCP resource 读取回调。
 * @param uri 借用的 URI buffer，仅在回调期间有效。
 * @param uri_len URI 字节数，不保证 NUL 结尾。
 * @param result 借用的输出 message；回调应写入 JSON result 对象。
 * @param userdata 注册 resource 时传入的用户指针。
 * @return `GALAY_OK` 表示读取成功；非 OK 会转换为 JSON-RPC internal error 响应。
 * @note 回调不拥有 uri/result，不得跨回调保存。
 */
typedef galay_status_t (*galay_mcp_resource_reader_fn)(const char* uri,
                                                       size_t uri_len,
                                                       galay_mcp_message_t* result,
                                                       void* userdata);

/**
 * @brief MCP prompt 获取回调。
 * @param name 借用的 prompt name buffer，仅在回调期间有效。
 * @param name_len name 字节数。
 * @param arguments 借用的 JSON arguments buffer，仅在回调期间有效。
 * @param arguments_len arguments 字节数。
 * @param result 借用的输出 message；回调应写入 JSON result 对象。
 * @param userdata 注册 prompt 时传入的用户指针。
 * @return `GALAY_OK` 表示生成成功；非 OK 会转换为 JSON-RPC internal error 响应。
 */
typedef galay_status_t (*galay_mcp_prompt_getter_fn)(const char* name,
                                                     size_t name_len,
                                                     const char* arguments,
                                                     size_t arguments_len,
                                                     galay_mcp_message_t* result,
                                                     void* userdata);

/**
 * @brief 返回通用状态码对应的 MCP 错误字符串。
 * @param status `galay_status_t` 状态码。
 * @return 永久有效的静态字符串。
 */
const char* galay_mcp_get_error(galay_status_t status);

/**
 * @brief 创建空 MCP message。
 * @param out 成功时返回 message；调用方用 `galay_mcp_message_destroy` 释放。
 * @return `GALAY_OK`、`GALAY_INVALID_ARGUMENT` 或 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_mcp_message_create(galay_mcp_message_t** out);

/**
 * @brief 销毁 MCP message。
 * @param message 可为 NULL；销毁后 data accessor 返回的借用 buffer 失效。
 */
void galay_mcp_message_destroy(galay_mcp_message_t* message);

/**
 * @brief 设置 message 的 JSON 内容。
 * @param message MCP message handle。
 * @param json NUL 结尾 JSON 文本；函数会复制内容。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note 本函数不验证 JSON 语义，只保存文本。
 */
galay_status_t galay_mcp_message_set_json(galay_mcp_message_t* message, const char* json);

/**
 * @brief 获取 message 内部 JSON buffer。
 * @param message MCP message handle。
 * @param data 输出借用 buffer。
 * @param data_len 输出字节数。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note 返回 buffer 不保证 NUL 结尾，只在 message 修改或销毁前有效。
 */
galay_status_t galay_mcp_message_data(const galay_mcp_message_t* message, const char** data, size_t* data_len);

/**
 * @brief 构造 JSON-RPC request。
 * @param message 输出 message。
 * @param id JSON-RPC id。
 * @param method 非空方法名，不能包含空格。
 * @param params 可选 JSON params 文本；NULL 表示不写 params 字段。
 * @return 成功返回 `GALAY_OK`；参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note params 会直接拼接，调用方负责保证其为合法 JSON。
 */
galay_status_t galay_mcp_build_request(galay_mcp_message_t* message, int64_t id, const char* method, const char* params);

/**
 * @brief 构造 JSON-RPC notification。
 * @param message 输出 message。
 * @param method 非空方法名，不能包含空格。
 * @param params 可选 JSON params 文本。
 * @return 成功返回 `GALAY_OK`。
 */
galay_status_t galay_mcp_build_notification(galay_mcp_message_t* message, const char* method, const char* params);

/**
 * @brief 构造 MCP initialized notification。
 * @param message 输出 message。
 * @return 成功返回 `GALAY_OK`。
 */
galay_status_t galay_mcp_build_initialized_notification(galay_mcp_message_t* message);

/**
 * @brief 构造空 result response。
 * @param message 输出 message。
 * @param id JSON-RPC id。
 * @return 成功返回 `GALAY_OK`。
 */
galay_status_t galay_mcp_build_empty_result_response(galay_mcp_message_t* message, int64_t id);

/**
 * @brief 解析 JSON-RPC request/notification。
 * @param data 输入 JSON buffer。
 * @param data_len 输入字节数。
 * @param out 成功时返回 parsed request；调用方 destroy。
 * @return 协议不匹配返回 `GALAY_PROTOCOL_ERROR`；失败时 `*out` 置 NULL。
 * @note 解析器为最小 JSON-RPC 解析器，保留 params 原始 JSON 片段。
 */
galay_status_t galay_mcp_parse_request(const char* data, size_t data_len, galay_mcp_parsed_request_t** out);

/**
 * @brief 销毁 parsed request。
 * @param request 可为 NULL；销毁后 method/params 借用 buffer 失效。
 */
void galay_mcp_parsed_request_destroy(galay_mcp_parsed_request_t* request);

/**
 * @brief 判断 request 是否为 notification。
 * @param request parsed request，可为 NULL。
 * @return notification 返回 `GALAY_TRUE`；NULL 或普通 request 返回 `GALAY_FALSE`。
 */
galay_bool_t galay_mcp_request_is_notification(const galay_mcp_parsed_request_t* request);

/**
 * @brief 获取 request id。
 * @param request parsed request。
 * @param id 输出 id。
 * @return notification 或参数无效返回错误。
 */
galay_status_t galay_mcp_request_id(const galay_mcp_parsed_request_t* request, int64_t* id);

/**
 * @brief 获取 request method。
 * @param request parsed request。
 * @param method 输出借用 method buffer。
 * @param method_len 输出字节数。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note buffer 只在 request destroy 前有效。
 */
galay_status_t galay_mcp_request_method(const galay_mcp_parsed_request_t* request, const char** method, size_t* method_len);

/**
 * @brief 获取 request params JSON。
 * @param request parsed request。
 * @param params 输出借用 params buffer；无 params 时长度为 0。
 * @param params_len 输出字节数。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mcp_request_params(const galay_mcp_parsed_request_t* request, const char** params, size_t* params_len);

/**
 * @brief 解析 JSON-RPC response。
 * @param data 输入 JSON buffer。
 * @param data_len 输入字节数。
 * @param out 成功时返回 parsed response；调用方 destroy。
 * @return error response 或协议不匹配返回 `GALAY_PROTOCOL_ERROR`。
 */
galay_status_t galay_mcp_parse_response(const char* data, size_t data_len, galay_mcp_parsed_response_t** out);

/**
 * @brief 销毁 parsed response。
 * @param response 可为 NULL；销毁后 result 借用 buffer 失效。
 */
void galay_mcp_parsed_response_destroy(galay_mcp_parsed_response_t* response);

/**
 * @brief 获取 response id。
 * @param response parsed response。
 * @param id 输出 id。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mcp_response_id(const galay_mcp_parsed_response_t* response, int64_t* id);

/**
 * @brief 判断 response 是否带非空 result。
 * @param response parsed response。
 * @return 有非空 result 返回 `GALAY_TRUE`。
 */
galay_bool_t galay_mcp_response_has_result(const galay_mcp_parsed_response_t* response);

/**
 * @brief 获取 response result JSON。
 * @param response parsed response。
 * @param result 输出借用 result buffer。
 * @param result_len 输出字节数。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note buffer 只在 response destroy 前有效。
 */
galay_status_t galay_mcp_response_result(const galay_mcp_parsed_response_t* response, const char** result, size_t* result_len);

/**
 * @brief 创建 STDIO/loopback client 配置。
 * @param command 预留字段；当前实现不启动外部进程。
 * @param args 预留字段；当前实现不解析参数。
 * @param out 成功时返回 config；调用方 destroy。
 * @return `GALAY_OK` 或错误状态。
 */
galay_status_t galay_mcp_stdio_config_create(const char* command, const char* args, galay_mcp_client_config_t** out);

/**
 * @brief 创建 HTTP client 配置。
 * @param url NUL 结尾 URL，当前实现要求 `http://host:port/path`。
 * @param out 成功时返回 config；调用方 destroy。
 * @return 参数无效或内存不足通过 `galay_status_t` 返回。
 */
galay_status_t galay_mcp_http_config_create(const char* url, galay_mcp_client_config_t** out);

/**
 * @brief 设置 HTTP bearer token。
 * @param config HTTP config。
 * @param bearer_token 非空 token；会被复制。
 * @return config 非 HTTP 或 token 无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mcp_http_config_set_bearer_token(galay_mcp_client_config_t* config,
                                                      const char* bearer_token);

/**
 * @brief 销毁 client config。
 * @param config 可为 NULL；不影响已创建 client。
 */
void galay_mcp_client_config_destroy(galay_mcp_client_config_t* config);

/**
 * @brief 获取 client config 模式。
 * @param config client config；NULL 时返回 STDIO。
 * @return 配置模式。
 */
galay_mcp_mode_t galay_mcp_client_config_mode(const galay_mcp_client_config_t* config);

/**
 * @brief 获取 HTTP config URL。
 * @param config HTTP config。
 * @param url 输出借用 URL buffer。
 * @param url_len 输出字节数。
 * @return 参数无效或非 HTTP config 返回 `GALAY_INVALID_ARGUMENT`。
 * @note URL buffer 只在 config destroy 前有效。
 */
galay_status_t galay_mcp_http_config_url(const galay_mcp_client_config_t* config, const char** url, size_t* url_len);

/**
 * @brief 创建 MCP client。
 * @param config client config；内容会复制到 client。
 * @param out 成功时返回 client；调用方 destroy。
 * @return config/URL 无效或分配失败通过 `galay_status_t` 返回。
 */
galay_status_t galay_mcp_client_create(const galay_mcp_client_config_t* config, galay_mcp_client_t** out);

/**
 * @brief 销毁 MCP client。
 * @param client 可为 NULL；不会销毁 loopback server。
 */
void galay_mcp_client_destroy(galay_mcp_client_t* client);

/**
 * @brief 查询 client 连接状态。
 * @param client MCP client。
 * @return connected 返回 `GALAY_TRUE`。
 */
galay_bool_t galay_mcp_client_is_connected(const galay_mcp_client_t* client);

/**
 * @brief 将 STDIO client 连接到本地 loopback server。
 * @param client STDIO client。
 * @param server STDIO server；client 借用该 server，server 必须更长寿。
 * @param timeout_ms 预留 timeout；当前 loopback 不执行阻塞 I/O。
 * @return 成功返回 `C_IOResultOk`；参数或模式错误返回 `C_IOResultInvalid`。
 */
C_IOResult galay_mcp_client_connect_stdio_loopback(galay_mcp_client_t* client,
                                                   galay_mcp_server_t* server,
                                                   int64_t timeout_ms);

/**
 * @brief 标记 client 已连接。
 * @param client MCP client。
 * @param timeout_ms HTTP 模式预留；实际请求时使用各 API timeout。
 * @return 成功返回 `C_IOResultOk`；配置不完整返回 `C_IOResultInvalid`。
 * @note HTTP 模式不会在此建立长连接，后续每次 request 使用 C coroutine TCP I/O。
 */
C_IOResult galay_mcp_client_connect_async(galay_mcp_client_t* client, int64_t timeout_ms);

/**
 * @brief 断开 MCP client。
 * @param client MCP client。
 * @param timeout_ms 预留 timeout。
 * @return 成功返回 `C_IOResultOk`。
 */
C_IOResult galay_mcp_client_disconnect_async(galay_mcp_client_t* client, int64_t timeout_ms);

/**
 * @brief 发送 MCP initialize。
 * @param client 已连接 MCP client。
 * @param client_name 非空 client 名称。
 * @param client_version 非空 client 版本。
 * @param timeout_ms 每次 HTTP socket I/O 超时；负数语义遵循 kernel TCP API。
 * @return `C_IOResultOk` 表示初始化成功；错误通过 `C_IOResult` 返回。
 * @note 除 initialize 外的 client request 要求先初始化。
 */
C_IOResult galay_mcp_client_initialize_async(galay_mcp_client_t* client,
                                             const char* client_name,
                                             const char* client_version,
                                             int64_t timeout_ms);

/**
 * @brief 发送 MCP ping。
 * @param client 已初始化 MCP client。
 * @param timeout_ms I/O 超时。
 * @return `C_IOResultOk` 表示收到成功 result。
 */
C_IOResult galay_mcp_client_ping_async(galay_mcp_client_t* client, int64_t timeout_ms);

/**
 * @brief 请求 tools/list。
 * @param client 已初始化 MCP client。
 * @param timeout_ms I/O 超时。
 * @param result 成功时返回 message；调用方用 `galay_mcp_message_destroy` 释放。
 * @return `C_IOResultOk` 表示 result message 有效；`C_IOResult.ptr` 指向同一 message。
 */
C_IOResult galay_mcp_client_list_tools_async(galay_mcp_client_t* client,
                                             int64_t timeout_ms,
                                             galay_mcp_message_t** result);

/**
 * @brief 请求 tools/call。
 * @param client 已初始化 MCP client。
 * @param tool_name tool 名称。
 * @param arguments_json 可选 arguments JSON；NULL 或空字符串使用 `{}`。
 * @param timeout_ms I/O 超时。
 * @param result 成功时返回 result message；调用方 destroy。
 * @return JSON-RPC error、协议错误、超时或 I/O 失败通过 `C_IOResult` 返回。
 */
C_IOResult galay_mcp_client_call_tool_async(galay_mcp_client_t* client,
                                            const char* tool_name,
                                            const char* arguments_json,
                                            int64_t timeout_ms,
                                            galay_mcp_message_t** result);

/**
 * @brief 请求 resources/list。
 * @param client 已初始化 MCP client。
 * @param timeout_ms I/O 超时。
 * @param result 成功时返回 message；调用方 destroy。
 * @return 成功返回 `C_IOResultOk`。
 */
C_IOResult galay_mcp_client_list_resources_async(galay_mcp_client_t* client,
                                                 int64_t timeout_ms,
                                                 galay_mcp_message_t** result);

/**
 * @brief 请求 resources/read。
 * @param client 已初始化 MCP client。
 * @param uri resource URI。
 * @param timeout_ms I/O 超时。
 * @param result 成功时返回 message；调用方 destroy。
 * @return 成功返回 `C_IOResultOk`。
 */
C_IOResult galay_mcp_client_read_resource_async(galay_mcp_client_t* client,
                                                const char* uri,
                                                int64_t timeout_ms,
                                                galay_mcp_message_t** result);

/**
 * @brief 请求 prompts/list。
 * @param client 已初始化 MCP client。
 * @param timeout_ms I/O 超时。
 * @param result 成功时返回 message；调用方 destroy。
 * @return 成功返回 `C_IOResultOk`。
 */
C_IOResult galay_mcp_client_list_prompts_async(galay_mcp_client_t* client,
                                               int64_t timeout_ms,
                                               galay_mcp_message_t** result);

/**
 * @brief 请求 prompts/get。
 * @param client 已初始化 MCP client。
 * @param name prompt 名称。
 * @param arguments_json 可选 arguments JSON；NULL 或空字符串使用 `{}`。
 * @param timeout_ms I/O 超时。
 * @param result 成功时返回 message；调用方 destroy。
 * @return 成功返回 `C_IOResultOk`。
 */
C_IOResult galay_mcp_client_get_prompt_async(galay_mcp_client_t* client,
                                             const char* name,
                                             const char* arguments_json,
                                             int64_t timeout_ms,
                                             galay_mcp_message_t** result);

/**
 * @brief 创建 STDIO/loopback server。
 * @param out 成功时返回 server；调用方用 `galay_mcp_server_destroy` 释放。
 * @return `GALAY_OK` 或错误状态。
 */
galay_status_t galay_mcp_stdio_server_create(galay_mcp_server_t** out);

/**
 * @brief 创建 HTTP server。
 * @param host 监听地址，不能为空。
 * @param port 监听端口；0 表示由系统分配。
 * @param out 成功时返回 server；调用方 destroy。
 * @return 参数无效或内存不足通过 `galay_status_t` 返回。
 */
galay_status_t galay_mcp_http_server_create(const char* host, uint16_t port, galay_mcp_server_t** out);

/**
 * @brief 销毁 MCP server。
 * @param server 可为 NULL；不会释放注册回调的 userdata。
 * @note 若 HTTP listener 仍存在，会先释放 listener handle。
 */
void galay_mcp_server_destroy(galay_mcp_server_t* server);

/**
 * @brief 设置 serverInfo。
 * @param server MCP server。
 * @param name 非空 server name；会被复制。
 * @param version 非空 server version；会被复制。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mcp_server_set_info(galay_mcp_server_t* server,
                                         const char* name,
                                         const char* version);

/**
 * @brief 为 HTTP server 设置 bearer token 要求。
 * @param server HTTP server。
 * @param bearer_token 非空 token；会被复制。
 * @return server 非 HTTP 或 token 无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mcp_http_server_require_bearer_token(galay_mcp_server_t* server,
                                                          const char* bearer_token);

/**
 * @brief 注册 MCP tool。
 * @param server MCP server。
 * @param name 非空 tool 名称；会被复制。
 * @param description 可选描述；NULL 视为空字符串。
 * @param input_schema_json 可选 JSON schema；NULL 或空字符串使用 `{}`。
 * @param handler tool 回调，不能为空。
 * @param userdata 透传给 handler，生命周期由调用方管理。
 * @return 成功返回 `GALAY_OK`。
 */
galay_status_t galay_mcp_server_add_tool(galay_mcp_server_t* server,
                                         const char* name,
                                         const char* description,
                                         const char* input_schema_json,
                                         galay_mcp_tool_handler_fn handler,
                                         void* userdata);

/**
 * @brief 注册 MCP resource。
 * @param server MCP server。
 * @param uri 非空 resource URI；会被复制。
 * @param name 非空 resource 名称；会被复制。
 * @param description 可选描述。
 * @param mime_type 可选 MIME；NULL 使用 `application/octet-stream`。
 * @param reader resource reader 回调，不能为空。
 * @param userdata 透传给 reader。
 * @return 成功返回 `GALAY_OK`。
 */
galay_status_t galay_mcp_server_add_resource(galay_mcp_server_t* server,
                                             const char* uri,
                                             const char* name,
                                             const char* description,
                                             const char* mime_type,
                                             galay_mcp_resource_reader_fn reader,
                                             void* userdata);

/**
 * @brief 注册 MCP prompt。
 * @param server MCP server。
 * @param name 非空 prompt 名称；会被复制。
 * @param description 可选描述。
 * @param arguments_json 可选参数 schema 数组；NULL 或空字符串使用 `[]`。
 * @param getter prompt getter 回调，不能为空。
 * @param userdata 透传给 getter。
 * @return 成功返回 `GALAY_OK`。
 */
galay_status_t galay_mcp_server_add_prompt(galay_mcp_server_t* server,
                                           const char* name,
                                           const char* description,
                                           const char* arguments_json,
                                           galay_mcp_prompt_getter_fn getter,
                                           void* userdata);

/**
 * @brief 启动 HTTP MCP server listener。
 * @param server HTTP server。
 * @return 成功返回 `GALAY_OK`；bind/listen 失败返回 I/O 错误。
 * @note port 为 0 时可通过 `galay_mcp_http_server_endpoint` 查询实际端口。
 */
galay_status_t galay_mcp_http_server_start(galay_mcp_server_t* server);

/**
 * @brief 获取 HTTP server 实际监听 endpoint。
 * @param server 已 start 的 HTTP server。
 * @param host 输出借用 host buffer。
 * @param port 输出监听端口。
 * @return server 未监听、非 HTTP 或参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note host buffer 只在 server destroy 或下一次 endpoint 修改前有效。
 */
galay_status_t galay_mcp_http_server_endpoint(const galay_mcp_server_t* server,
                                              const char** host,
                                              uint16_t* port);

/**
 * @brief 接收并处理一个 HTTP MCP 请求。
 * @param server 已 start 的 HTTP server。
 * @param timeout_ms accept/read/write 的 I/O 超时。
 * @return 成功处理并发送响应返回 `C_IOResultOk`；超时、取消、I/O 或协议错误通过返回值传播。
 * @note 必须在 C coroutine/runtime 支持的上下文中调用；函数处理一个连接后关闭 session。
 */
C_IOResult galay_mcp_http_server_serve_once(galay_mcp_server_t* server, int64_t timeout_ms);

/**
 * @brief 停止 HTTP MCP server listener。
 * @param server HTTP server。
 * @return 成功返回 `C_IOResultOk`；参数或 listener destroy 失败通过 `C_IOResult` 返回。
 * @note stop 不销毁 server，也不释放注册表。
 */
C_IOResult galay_mcp_http_server_stop(galay_mcp_server_t* server);

#ifdef __cplusplus
}
#endif

#endif
