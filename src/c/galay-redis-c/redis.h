#ifndef GALAY_C_REDIS_REDIS_H
#define GALAY_C_REDIS_REDIS_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Redis RESP reply 类型。
 * @details 枚举覆盖 RESP2 与当前 C parser 支持的 RESP3 类型；访问 reply 内容前应先
 *          调用 `galay_redis_reply_type`，再选择匹配的 accessor。
 * @note 类型不匹配时 accessor 返回 `GALAY_INVALID_ARGUMENT`，不会做隐式转换。
 */
typedef enum galay_redis_resp_type_t {
    GALAY_REDIS_RESP_SIMPLE_STRING = 0,  ///< RESP simple string。
    GALAY_REDIS_RESP_ERROR = 1,          ///< RESP error；可用于 cluster redirect 解析。
    GALAY_REDIS_RESP_INTEGER = 2,        ///< RESP integer。
    GALAY_REDIS_RESP_BULK_STRING = 3,    ///< RESP bulk string。
    GALAY_REDIS_RESP_ARRAY = 4,          ///< RESP array。
    GALAY_REDIS_RESP_NIL = 5,            ///< Null bulk string 或 null array。
    GALAY_REDIS_RESP_DOUBLE = 6,         ///< RESP3 double。
    GALAY_REDIS_RESP_BOOLEAN = 7,        ///< RESP3 boolean。
    GALAY_REDIS_RESP_BLOB_ERROR = 8,     ///< RESP3 blob error。
    GALAY_REDIS_RESP_VERBATIM_STRING = 9,///< RESP3 verbatim string。
    GALAY_REDIS_RESP_BIG_NUMBER = 10,    ///< RESP3 big number，作为字符串读取。
    GALAY_REDIS_RESP_MAP = 11,           ///< RESP3 map。
    GALAY_REDIS_RESP_SET = 12,           ///< RESP3 set，按数组读取。
    GALAY_REDIS_RESP_PUSH = 13           ///< RESP3 push，按数组读取。
} galay_redis_resp_type_t;

/**
 * @brief Redis Cluster 重定向类型。
 * @details route 查询返回 `NONE`；解析 `-MOVED` 或 `-ASK` reply 时返回对应重定向类型。
 */
typedef enum galay_redis_redirect_type_t {
    GALAY_REDIS_REDIRECT_NONE = 0,   ///< 普通 slot 路由。
    GALAY_REDIS_REDIRECT_MOVED = 1,  ///< MOVED，调用会更新本地 slot 路由缓存。
    GALAY_REDIS_REDIRECT_ASK = 2     ///< ASK，调用只返回临时目标，不更新缓存。
} galay_redis_redirect_type_t;

/**
 * @brief RESP 命令编码器 opaque handle。
 * @details handle 拥有最近一次编码得到的内部字符串，`build` 返回的 buffer 指针借用该字符串。
 * @note 非线程安全；同一 builder 不应被多个线程或多个 C coroutine 并发使用。
 */
typedef struct galay_redis_command_builder_t galay_redis_command_builder_t;

/**
 * @brief Redis reply opaque handle。
 * @details 由 parser 或 async command 返回，拥有完整 reply 树；子 reply 和字符串 view
 *          均借用父 reply 内部存储。
 * @note 调用方必须用 `galay_redis_reply_destroy` 或 `galay_redis_reply_free` 释放根 reply。
 */
typedef struct galay_redis_reply_t galay_redis_reply_t;

/**
 * @brief Redis standalone client opaque handle。
 * @details client 独占底层 TCP socket 和 receive buffer；async I/O API 只应在同一个
 *          galay C coroutine/runtime 上串行调用。
 * @note handle 非线程安全；销毁或 close 前调用方必须保证没有挂起的 connect/command/close。
 */
typedef struct galay_redis_client_t galay_redis_client_t;

/**
 * @brief Redis pipeline 命令缓存 opaque handle。
 * @details pipeline 拥有已编码命令副本；添加命令后输入参数可立即释放。
 * @note pipeline 只缓存请求，不拥有 reply；reply 数组由 `galay_redis_client_pipeline_async`
 *       返回并由 `galay_redis_pipeline_replies_destroy` 释放。
 */
typedef struct galay_redis_pipeline_t galay_redis_pipeline_t;

/**
 * @brief Redis 连接池 opaque handle。
 * @details pool 拥有连接池内 client；lease 期间 client 借给调用方，release 后不得继续使用。
 * @note 当前实现不提供锁保护，不支持跨线程并发 acquire/release。
 */
typedef struct galay_redis_pool_t galay_redis_pool_t;

/**
 * @brief Redis pool lease opaque handle。
 * @details lease 表示一次连接借出；必须归还给创建它的 pool。
 * @note release 会销毁 lease handle；release 后不得再访问 lease 或其中 client。
 */
typedef struct galay_redis_pool_lease_t galay_redis_pool_lease_t;

/**
 * @brief Redis Cluster 路由表 opaque handle。
 * @details cluster 保存节点配置和 MOVED 更新后的本地 slot 路由缓存。
 * @note route 中的 host 指针借用 cluster 内部字符串，下一次修改 cluster 或销毁后失效。
 */
typedef struct galay_redis_cluster_t galay_redis_cluster_t;

/**
 * @brief Redis client 配置。
 * @details `host == NULL` 或空字符串使用默认 `127.0.0.1`；`port == 0` 使用默认 6379；
 *          `connect_timeout_ms` 在 connect 调用传入负数 timeout 时作为有效超时。
 * @note `username` 非空时 `password` 必须非空；字符串只在 create 调用期间借用并被复制到 client。
 */
typedef struct galay_redis_client_config_t {
    const char* host;          ///< Redis host，NULL/空字符串使用默认值。
    uint16_t port;             ///< Redis port，0 使用默认值。
    const char* username;      ///< Redis 6 ACL 用户名，可为 NULL。
    const char* password;      ///< 密码；username 非空时必须非空。
    int db_index;              ///< 目标 database index，仅作为 client 配置保存。
    int resp_version;          ///< 期望 RESP 版本，仅作为 client 配置保存。
    int connect_timeout_ms;    ///< 默认连接超时，负数表示无限等待。
} galay_redis_client_config_t;

/**
 * @brief Redis pool 配置。
 * @details pool 会复制 `client` 中的字符串配置；`max_connections` 必须大于 0，且
 *          `min_connections <= max_connections`、`initial_connections <= max_connections`。
 * @note `initial_connections` 只预创建 client handle，不预连接；连接在 acquire 时建立。
 */
typedef struct galay_redis_pool_config_t {
    galay_redis_client_config_t client; ///< 连接配置，字符串在 create 时复制。
    size_t min_connections;             ///< 当前实现保留字段，不主动维持最小连接数。
    size_t max_connections;             ///< 最大连接数，必须大于 0。
    size_t initial_connections;         ///< 初始 client handle 数量。
} galay_redis_pool_config_t;

/**
 * @brief Redis Cluster 节点配置。
 * @details slot 范围为闭区间，必须满足 `slot_start <= slot_end <= 16383`。
 * @note `host` 在 add_node 调用期间借用并复制到 cluster。
 */
typedef struct galay_redis_cluster_node_config_t {
    const char* host;       ///< 节点 host，不能为空。
    uint16_t port;          ///< 节点 port，必须非 0。
    uint16_t slot_start;    ///< 起始 slot。
    uint16_t slot_end;      ///< 结束 slot。
} galay_redis_cluster_node_config_t;

/**
 * @brief Redis Cluster 路由查询结果。
 * @details `node_index` 对普通/MOVED 路由指向 cluster 内部节点数组；ASK 临时路由使用
 *          `SIZE_MAX` 作为非缓存节点标识。
 * @note `host` 为借用指针；普通/MOVED 路由在 cluster 下次修改或销毁前有效，ASK 路由在
 *       下一次 redirect 解析或 cluster 销毁前有效。
 */
typedef struct galay_redis_cluster_route_t {
    uint16_t slot;                              ///< 命中的 slot。
    size_t node_index;                          ///< 节点索引；ASK 路由为 `SIZE_MAX`。
    const char* host;                           ///< 借用 host 指针。
    uint16_t port;                              ///< 节点 port。
    galay_redis_redirect_type_t redirect_type;  ///< 路由来源或重定向类型。
} galay_redis_cluster_route_t;

/**
 * @brief 将 Redis C ABI 状态码转换为静态错误字符串。
 * @param status `galay_status_t` 状态码。
 * @return 指向静态字符串的指针，调用方不得释放。
 * @note async API 使用 `C_IOResult.code` 表达 I/O 状态；当 `value` 保存 `galay_status_t`
 *       时可用本函数解释该值。
 */
const char* galay_redis_get_error(galay_status_t status);

/**
 * @brief 创建 RESP 命令编码器。
 * @param out 成功时返回 builder 所有权，调用方用 `galay_redis_command_builder_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；`out == NULL` 返回 `GALAY_INVALID_ARGUMENT`，分配失败返回
 *         `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_redis_command_builder_create(galay_redis_command_builder_t** out);

/**
 * @brief 销毁 RESP 命令编码器。
 * @param builder 可为 NULL；销毁后先前 build 返回的 encoded 指针立即失效。
 * @note 该函数不阻塞，不访问网络。
 */
void galay_redis_command_builder_destroy(galay_redis_command_builder_t* builder);

/**
 * @brief 将命令和参数编码为 Redis RESP array。
 * @details `command` 和 `args` 仅在调用期间借用；编码结果存储在 builder 内部。
 * @param builder 由 `galay_redis_command_builder_create` 创建的 builder。
 * @param command Redis 命令名，不能为空。
 * @param args 参数指针数组；`arg_count == 0` 时可为 NULL。
 * @param arg_lens 每个参数长度；为 NULL 时按 C 字符串计算长度。
 * @param arg_count 参数数量。
 * @param encoded 成功时返回借用 buffer 指针。
 * @param encoded_len 成功时返回 buffer 字节数。
 * @return `GALAY_OK` 表示编码成功；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note `encoded` 生命周期到下一次 build、builder destroy 或 builder 被并发修改为止。
 */
galay_status_t galay_redis_command_builder_build(galay_redis_command_builder_t* builder,
                                                 const char* command,
                                                 const char* const* args,
                                                 const size_t* arg_lens,
                                                 size_t arg_count,
                                                 const char** encoded,
                                                 size_t* encoded_len);

/**
 * @brief 解析一段 RESP buffer。
 * @details 成功时只消费一个完整 reply，并通过 `consumed` 返回已消费字节数；多余字节留给调用方。
 * @param data RESP 字节序列，调用期间借用。
 * @param data_len buffer 字节数。
 * @param out 成功时返回 reply 所有权，调用方用 `galay_redis_reply_destroy` 释放。
 * @param consumed 成功时返回消费字节数。
 * @return `GALAY_OK` 表示解析成功；截断输入通常返回 `GALAY_INVALID_ARGUMENT`，非法协议返回
 *         `GALAY_PROTOCOL_ERROR`，分配失败返回 `GALAY_OUT_OF_MEMORY`。
 * @note reply 内部会复制字符串/子节点，不借用输入 `data`。
 */
galay_status_t galay_redis_parse_reply(const char* data, size_t data_len,
                                       galay_redis_reply_t** out, size_t* consumed);

/**
 * @brief 释放 Redis reply 树。
 * @param reply 可为 NULL；会递归释放数组/map 子节点。
 * @note 释放后所有通过 accessor 取得的字符串、子 reply 指针同时失效。
 */
void galay_redis_reply_destroy(galay_redis_reply_t* reply);

/**
 * @brief `galay_redis_reply_destroy` 的兼容别名。
 * @param reply 可为 NULL。
 * @note 供 C 调用方按 free 命名习惯释放 reply；语义与 destroy 完全一致。
 */
void galay_redis_reply_free(galay_redis_reply_t* reply);

/**
 * @brief 查询 reply 类型。
 * @param reply reply handle；NULL 时返回 `GALAY_REDIS_RESP_ERROR`。
 * @return RESP 类型枚举。
 * @note 返回值必须由调用方检查后再调用对应 accessor。
 */
galay_redis_resp_type_t galay_redis_reply_type(const galay_redis_reply_t* reply);

/**
 * @brief 获取字符串类 reply 的内容。
 * @details 支持 simple string、error、bulk string、blob error、verbatim string、big number。
 * @param reply reply handle。
 * @param value 成功时返回借用字符串指针；内容不保证 NUL 结尾。
 * @param value_len 成功时返回字节数。
 * @return 成功返回 `GALAY_OK`；类型不匹配或参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 指针生命周期到 reply destroy 为止。
 */
galay_status_t galay_redis_reply_string(const galay_redis_reply_t* reply, const char** value,
                                        size_t* value_len);

/**
 * @brief 获取 integer reply 的值。
 * @param reply reply handle。
 * @param value 输出整数。
 * @return 成功返回 `GALAY_OK`；类型不匹配或参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_redis_reply_integer(const galay_redis_reply_t* reply, int64_t* value);

/**
 * @brief 获取 RESP3 double reply 的值。
 * @param reply reply handle。
 * @param value 输出 double。
 * @return 成功返回 `GALAY_OK`；类型不匹配或参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_redis_reply_double(const galay_redis_reply_t* reply, double* value);

/**
 * @brief 获取 RESP3 boolean reply 的值。
 * @param reply reply handle。
 * @param value 输出 `GALAY_TRUE` 或 `GALAY_FALSE`。
 * @return 成功返回 `GALAY_OK`；类型不匹配或参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_redis_reply_boolean(const galay_redis_reply_t* reply, galay_bool_t* value);

/**
 * @brief 获取数组类 reply 的元素数量。
 * @details 支持 RESP array、set 和 push。
 * @param reply reply handle。
 * @param size 输出元素数量。
 * @return 成功返回 `GALAY_OK`；类型不匹配或参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_redis_reply_array_size(const galay_redis_reply_t* reply, size_t* size);

/**
 * @brief 获取数组类 reply 的指定子 reply。
 * @param reply reply handle。
 * @param index 元素索引。
 * @param out 成功时返回借用子 reply 指针。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回
 *         `GALAY_INVALID_ARGUMENT`。
 * @note `out` 指针由父 reply 拥有，不得单独 destroy。
 */
galay_status_t galay_redis_reply_array_at(const galay_redis_reply_t* reply, size_t index,
                                          const galay_redis_reply_t** out);

/**
 * @brief 获取 map reply 的键值对数量。
 * @param reply reply handle。
 * @param size 输出键值对数量。
 * @return 成功返回 `GALAY_OK`；类型不匹配或参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_redis_reply_map_size(const galay_redis_reply_t* reply, size_t* size);

/**
 * @brief 获取 map reply 中指定索引的 key/value 子 reply。
 * @param reply reply handle。
 * @param index 键值对索引。
 * @param key 成功时返回借用 key 子 reply。
 * @param value 成功时返回借用 value 子 reply。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回
 *         `GALAY_INVALID_ARGUMENT`。
 * @note key/value 由父 reply 拥有，不得单独 destroy。
 */
galay_status_t galay_redis_reply_map_at(const galay_redis_reply_t* reply, size_t index,
                                        const galay_redis_reply_t** key,
                                        const galay_redis_reply_t** value);

/**
 * @brief 创建 Redis client handle。
 * @details 成功后复制配置字符串，但不会自动连接、AUTH 或 SELECT。
 * @param config 连接配置；NULL 使用默认 standalone 配置。
 * @param out 成功时返回 client 所有权，调用方用 `galay_redis_client_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`，分配失败返回
 *         `GALAY_OUT_OF_MEMORY`。
 * @note client 非线程安全；所有 I/O API 应在同一 C coroutine 中串行调用。
 */
galay_status_t galay_redis_client_create(const galay_redis_client_config_t* config,
                                         galay_redis_client_t** out);

/**
 * @brief 销毁 Redis client handle。
 * @details 若内部 socket 仍存在，会先释放 socket 资源。
 * @param client 可为 NULL。
 * @note 该函数不等待未完成 I/O；调用方必须先确保没有挂起的 async 操作。
 */
void galay_redis_client_destroy(galay_redis_client_t* client);

/**
 * @brief 同步释放 client 内部 socket 并标记未连接。
 * @param client Redis client handle。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`，socket destroy 失败返回
 *         `GALAY_IO_ERROR`。
 * @note 这是资源释放接口，不执行 Redis QUIT；不会挂起 coroutine。
 */
galay_status_t galay_redis_client_disconnect(galay_redis_client_t* client);

/**
 * @brief 同步命令接口占位。
 * @details 当前实现不提供同步 Redis 命令发送。
 * @param client Redis client handle。
 * @param command Redis 命令名。
 * @param args 参数数组。
 * @param arg_lens 参数长度数组，可为 NULL。
 * @param arg_count 参数数量。
 * @param reply 输出 reply 所有权。
 * @return 当前对有效参数返回 `GALAY_UNSUPPORTED`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 使用 `galay_redis_client_command_async` 执行真实 I/O。
 */
galay_status_t galay_redis_client_command(galay_redis_client_t* client, const char* command,
                                          const char* const* args, const size_t* arg_lens,
                                          size_t arg_count, galay_redis_reply_t** reply);

/**
 * @brief 创建 Redis pipeline 命令缓存。
 * @details pipeline 只保存已编码请求，不打开连接，也不拥有任何 reply。
 * @param out 成功时返回 pipeline，调用方负责用 `galay_redis_pipeline_destroy` 释放。
 * @return `GALAY_OK` 表示创建成功；无效参数或内存不足通过 `galay_status_t` 返回。
 * @note pipeline 非线程安全；应在同一调度上下文内构建和提交。
 */
galay_status_t galay_redis_pipeline_create(galay_redis_pipeline_t** out);

/**
 * @brief 销毁 Redis pipeline 命令缓存。
 * @param pipeline 可为 NULL；销毁后其中缓存的命令不可再使用。
 * @note 不会释放已经提交后返回的 reply；reply 数组必须单独销毁。
 */
void galay_redis_pipeline_destroy(galay_redis_pipeline_t* pipeline);

/**
 * @brief 向 pipeline 追加一条 Redis 命令。
 * @details 命令会立即编码并复制到 pipeline 内部；输入字符串在函数返回后可释放。
 * @param pipeline 由 `galay_redis_pipeline_create` 创建的 pipeline。
 * @param command Redis 命令名，例如 "PING"。
 * @param args 命令参数数组；`arg_count` 为 0 时可为 NULL。
 * @param arg_lens 每个参数长度；为 NULL 时按 C 字符串长度计算。
 * @param arg_count 参数数量。
 * @return `GALAY_OK` 表示命令已编码并缓存；无效参数或编码失败通过返回值传播。
 * @note 该函数不访问网络；实际写入发生在 `galay_redis_client_pipeline_async`。
 */
galay_status_t galay_redis_pipeline_add_command(galay_redis_pipeline_t* pipeline,
                                                const char* command,
                                                const char* const* args,
                                                const size_t* arg_lens,
                                                size_t arg_count);

/**
 * @brief 释放 pipeline async 调用返回的 reply 数组及数组内每个 reply。
 * @param replies `galay_redis_client_pipeline_async` 成功返回的数组，可为 NULL。
 * @param reply_count 数组元素数量。
 * @note 只能用于释放 pipeline async 返回的数组；单个 command 返回的 reply 用
 *       `galay_redis_reply_destroy`。
 */
void galay_redis_pipeline_replies_destroy(galay_redis_reply_t** replies, size_t reply_count);

/**
 * @brief 在当前 C coroutine 内异步连接 Redis standalone 节点。
 * @details 创建并连接底层 TCP socket；成功后 `C_IOResult.ptr` 指向 client。
 * @param client 由 `galay_redis_client_create` 创建的 client。
 * @param timeout_ms 负数无限等待，0 直接超时，正数为毫秒超时。
 * @return `C_IOResultOk` 表示连接成功；参数无效、未在 C coroutine 内调用或超时通过
 *         `C_IOResult` 返回。该函数会挂起当前 C coroutine，不阻塞线程。
 * @note 当传入负数且 config 中 `connect_timeout_ms > 0` 时使用配置超时；无独立 cancel API，
 *       可通过 timeout 或关闭 runtime/socket 终止等待。
 */
C_IOResult galay_redis_client_connect(galay_redis_client_t* client, int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 内发送一条 Redis 命令并等待一个 RESP reply。
 * @details 命令参数只在调用期间借用；返回的 reply 拥有解析后的完整数据树。
 * @param client 已连接的 Redis client。
 * @param command Redis 命令名，例如 "PING"。
 * @param args 命令参数数组；`arg_count` 为 0 时可为 NULL。
 * @param arg_lens 每个参数长度；为 NULL 时按 C 字符串长度计算。
 * @param arg_count 参数数量。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param reply 成功时返回 reply，调用方负责用 `galay_redis_reply_destroy` 释放。
 * @return `C_IOResultOk` 表示命令发送和 reply 解析成功；错误通过返回值显式传播。
 * @note 该函数挂起当前 C coroutine，不阻塞线程；client receive buffer 由 client 独占，
 *       因此同一 client 不得并发执行多个 command。
 */
C_IOResult galay_redis_client_command_async(galay_redis_client_t* client,
                                            const char* command,
                                            const char* const* args,
                                            const size_t* arg_lens,
                                            size_t arg_count,
                                            int64_t timeout_ms,
                                            galay_redis_reply_t** reply);

/**
 * @brief 在当前 C coroutine 内发送 AUTH 并等待 `+OK`。
 * @details `username` 为空时发送旧式 `AUTH password`，否则发送 ACL 形式
 *          `AUTH username password`。
 * @param client 已连接的 Redis client。
 * @param username Redis 6 ACL 用户名；为 NULL 或空字符串时发送旧式 `AUTH password`。
 * @param password 认证密码，不能为 NULL。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @return `C_IOResultOk` 表示认证成功；无效参数、网络错误或非 OK reply 通过返回值传播。
 * @note 会挂起当前 C coroutine；函数内部会释放临时 reply。
 */
C_IOResult galay_redis_client_auth(galay_redis_client_t* client,
                                   const char* username,
                                   const char* password,
                                   int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 内发送 SELECT 并等待 `+OK`。
 * @param client 已连接的 Redis client。
 * @param db_index Redis database index，必须大于等于 0。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @return `C_IOResultOk` 表示切库成功；无效参数、网络错误或非 OK reply 通过返回值传播。
 * @note 会挂起当前 C coroutine；db index 不会自动写回 client 配置字段。
 */
C_IOResult galay_redis_client_select(galay_redis_client_t* client,
                                     int db_index,
                                     int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 内发送 pipeline 中的所有命令并保留每个 reply。
 * @details 按添加顺序写入全部命令，再按顺序读取同等数量 reply。
 * @param client 已连接的 Redis client。
 * @param pipeline 由 `galay_redis_pipeline_create` 创建并添加命令的 pipeline。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param replies 成功时返回 reply 指针数组，调用方负责用
 *        `galay_redis_pipeline_replies_destroy` 释放。
 * @param reply_count 成功时返回 reply 数量。
 * @return `C_IOResultOk` 表示所有命令写入且读到同等数量 reply；错误通过返回值传播。
 * @note pipeline 为空会返回 invalid；失败时内部会释放已解析的临时 reply 数组。
 */
C_IOResult galay_redis_client_pipeline_async(galay_redis_client_t* client,
                                             const galay_redis_pipeline_t* pipeline,
                                             int64_t timeout_ms,
                                             galay_redis_reply_t*** replies,
                                             size_t* reply_count);

/**
 * @brief 创建 Redis C 连接池。
 * @details pool 拥有内部 client；`initial_connections` 只创建 client handle，实际连接在 acquire 时建立。
 * @param config 连接和池大小配置；NULL 使用 127.0.0.1:6379、最大 1 连接。
 * @param out 成功时返回 pool，调用方负责用 `galay_redis_pool_destroy` 释放。
 * @return 参数非法或分配失败通过 `galay_status_t` 返回。
 * @note pool 不提供跨线程同步；应在同一 C runtime/调度上下文内 acquire/release。
 */
galay_status_t galay_redis_pool_create(const galay_redis_pool_config_t* config,
                                       galay_redis_pool_t** out);

/**
 * @brief 销毁 Redis C 连接池及其空闲/占用连接句柄。
 * @param pool 可为 NULL；调用方必须先停止使用所有 lease。
 * @note destroy 不会等待借出的 lease 归还；仍在使用的 lease/client 会成为悬空指针。
 */
void galay_redis_pool_destroy(galay_redis_pool_t* pool);

/**
 * @brief 在当前 C coroutine 内获取一个 Redis 连接 lease。
 * @details 复用空闲连接或在未达到上限时创建新 client；未连接 client 会在本调用内连接。
 * @param pool 由 `galay_redis_pool_create` 创建的连接池。
 * @param timeout_ms 新建连接时使用的连接超时；连接池已满时返回 `C_IOResultTimeout`。
 * @param lease 成功时返回 lease，必须用 `galay_redis_pool_release` 归还。
 * @return 成功返回 `C_IOResultOk`；参数错误、连接失败或池耗尽通过返回值传播。
 * @note 会挂起当前 C coroutine；pool 不提供等待队列，达到上限会立即返回 timeout。
 */
C_IOResult galay_redis_pool_acquire(galay_redis_pool_t* pool,
                                    int64_t timeout_ms,
                                    galay_redis_pool_lease_t** lease);

/**
 * @brief 归还一个 Redis pool lease。
 * @param pool lease 所属 pool。
 * @param lease 由 `galay_redis_pool_acquire` 返回的 lease。
 * @return `GALAY_OK` 表示归还成功；重复归还或 pool 不匹配返回错误。
 * @note 成功 release 会销毁 lease handle；调用方不得再访问该 lease。
 */
galay_status_t galay_redis_pool_release(galay_redis_pool_t* pool,
                                        galay_redis_pool_lease_t* lease);

/**
 * @brief 获取 lease 中借用的 client。
 * @param lease 由 `galay_redis_pool_acquire` 返回且尚未 release 的 lease。
 * @return 返回的 client 由 pool 拥有，生命周期到 lease release 为止；调用方不得 destroy。
 * @note lease 无效时返回 NULL；client 仍遵循非线程安全、串行 I/O 的约束。
 */
galay_redis_client_t* galay_redis_pool_lease_client(galay_redis_pool_lease_t* lease);

/**
 * @brief 创建最小 Redis Cluster 路由表。
 * @param out 成功时返回 cluster，调用方负责用 `galay_redis_cluster_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 * @note cluster 只维护本地路由表，不主动连接 Redis 节点。
 */
galay_status_t galay_redis_cluster_create(galay_redis_cluster_t** out);

/**
 * @brief 销毁 Redis Cluster 路由表。
 * @param cluster 可为 NULL。
 * @note 销毁后所有 route.host 借用指针失效。
 */
void galay_redis_cluster_destroy(galay_redis_cluster_t* cluster);

/**
 * @brief 添加一个节点及其 slot 覆盖范围。
 * @details 新节点会追加到路由表；slot 查询按后添加优先，因此 MOVED 更新可覆盖旧范围。
 * @param cluster Redis Cluster 路由表。
 * @param node host/port 和闭区间 slot range；host 字符串会被复制到 cluster。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 本函数不校验与已有节点的 slot 重叠。
 */
galay_status_t galay_redis_cluster_add_node(galay_redis_cluster_t* cluster,
                                            const galay_redis_cluster_node_config_t* node);

/**
 * @brief 计算 Redis Cluster key slot。
 * @param key key 字节序列；支持 `{tag}` hash tag。
 * @param key_len key 字节长度。
 * @param slot 成功时返回 0-16383。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note `key` 只在调用期间借用，可包含 NUL 字节。
 */
galay_status_t galay_redis_cluster_key_slot(const char* key, size_t key_len, uint16_t* slot);

/**
 * @brief 按 slot 查询当前路由节点。
 * @param cluster Redis Cluster 路由表。
 * @param slot 目标 slot，必须小于等于 16383。
 * @param route 成功时返回借用 host 指针；指针在下一次 cluster 修改前有效。
 * @return 命中返回 `GALAY_OK`；无节点覆盖返回 `GALAY_NOT_FOUND`；参数非法返回
 *         `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_redis_cluster_route_slot(const galay_redis_cluster_t* cluster,
                                              uint16_t slot,
                                              galay_redis_cluster_route_t* route);

/**
 * @brief 按 key 计算 slot 并查询当前路由节点。
 * @param cluster Redis Cluster 路由表。
 * @param key key 字节序列。
 * @param key_len key 字节长度。
 * @param route 成功时返回路由信息。
 * @return 透传 `galay_redis_cluster_key_slot` 与 `galay_redis_cluster_route_slot` 的结果。
 * @note route.host 的生命周期与 `galay_redis_cluster_route_slot` 相同。
 */
galay_status_t galay_redis_cluster_route_key(const galay_redis_cluster_t* cluster,
                                             const char* key,
                                             size_t key_len,
                                             galay_redis_cluster_route_t* route);

/**
 * @brief 解析 `-MOVED` 或 `-ASK` reply 并返回目标路由。
 * @details MOVED 会更新 slot 路由缓存；ASK 只返回临时目标，不修改缓存。
 * @param cluster Redis Cluster 路由表。
 * @param reply Redis error reply，内容形如 `MOVED slot host:port` 或 `ASK slot host:port`。
 * @param route 成功时返回重定向目标。
 * @return 成功返回 `GALAY_OK`；reply 类型不匹配、payload 非法或节点配置非法返回错误状态。
 * @note 对 ASK 返回的 route.host 借用 cluster 内部临时字符串，下一次 redirect 解析后失效。
 */
galay_status_t galay_redis_cluster_apply_redirect(galay_redis_cluster_t* cluster,
                                                  const galay_redis_reply_t* reply,
                                                  galay_redis_cluster_route_t* route);

/**
 * @brief 在当前 C coroutine 内关闭 Redis TCP 连接并释放内部 socket。
 * @details 调用 kernel TCP close awaitable 后销毁 socket，并清空 client receive buffer。
 * @param client 已连接或持有 socket 的 Redis client。
 * @param timeout_ms 关闭操作超时，语义同 kernel TCP close。
 * @return `C_IOResultOk` 表示关闭并清理成功；失败通过返回值传播。
 * @note close 会挂起当前 C coroutine；成功或失败后 client 都会标记为未连接。
 */
C_IOResult galay_redis_client_close(galay_redis_client_t* client, int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
