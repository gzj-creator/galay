/**
 * @file etcd.h
 * @brief Etcd C ABI 客户端、pipeline、watch 与 lease 接口。
 * @details 本头文件只暴露不透明 handle 和显式错误码；所有对象所有权通过
 * create/destroy 或结果 destroy 函数传递，不通过异常传播失败。
 */
#ifndef GALAY_C_ETCD_ETCD_H
#define GALAY_C_ETCD_ETCD_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Etcd C ABI 细分错误码。
 * @details API 同时返回 `galay_status_t` 作为通用 C 状态；需要 Etcd 语义时，
 * 调用方可传入 `galay_etcd_error_code_t* code` 获取更具体的失败原因。
 */
typedef enum galay_etcd_error_code_t {
    GALAY_ETCD_ERROR_SUCCESS = 0,           ///< 操作成功。
    GALAY_ETCD_ERROR_INVALID_ENDPOINT = 1,  ///< endpoint 格式或地址无效。
    GALAY_ETCD_ERROR_INVALID_ARGUMENT = 2,  ///< 入参为空、长度无效或枚举值非法。
    GALAY_ETCD_ERROR_NOT_CONNECTED = 3,     ///< client 尚未连接或已关闭。
    GALAY_ETCD_ERROR_IO = 4,                ///< TCP/HTTP I/O 失败或服务端返回非 2xx。
    GALAY_ETCD_ERROR_PROTOCOL = 5,          ///< HTTP/JSON 响应不符合当前最小解析器预期。
    GALAY_ETCD_ERROR_CANCELLED = 6          ///< watch 已被取消。
} galay_etcd_error_code_t;

/**
 * @brief 多 endpoint 选择策略。
 * @details 当前 C ABI 保存策略值供 client 配置使用；实际可用性仍取决于实现对
 * endpoint 列表和健康检查的支持。
 */
typedef enum galay_etcd_endpoint_policy_t {
    GALAY_ETCD_ENDPOINT_POLICY_FIRST_HEALTHY = 0, ///< 优先选择第一个健康 endpoint。
    GALAY_ETCD_ENDPOINT_POLICY_ROUND_ROBIN = 1,   ///< 轮询 endpoint。
    GALAY_ETCD_ENDPOINT_POLICY_STICKY_LEADER = 2  ///< 尽量粘滞到 leader endpoint。
} galay_etcd_endpoint_policy_t;

/**
 * @brief Pipeline 结果条目的操作类型。
 * @details 调用 `galay_etcd_pipeline_result_item_type` 后，应按类型选择对应的
 * item accessor；类型不匹配会返回 `GALAY_INVALID_ARGUMENT`。
 */
typedef enum galay_etcd_pipeline_op_type_t {
    GALAY_ETCD_PIPELINE_PUT = 0,    ///< 写入 key/value。
    GALAY_ETCD_PIPELINE_GET = 1,    ///< 读取 key/value。
    GALAY_ETCD_PIPELINE_DELETE = 2  ///< 删除 key/value。
} galay_etcd_pipeline_op_type_t;

/**
 * @brief Watch 事件类型。
 * @details `UNKNOWN` 表示响应中没有可识别的事件类型；调用方仍可读取事件内的
 * watch_id、key 和 value 视图。
 */
typedef enum galay_etcd_watch_event_type_t {
    GALAY_ETCD_WATCH_EVENT_UNKNOWN = 0, ///< 未识别或空事件。
    GALAY_ETCD_WATCH_EVENT_PUT = 1,     ///< PUT 事件。
    GALAY_ETCD_WATCH_EVENT_DELETE = 2   ///< DELETE 事件。
} galay_etcd_watch_event_type_t;

/**
 * @brief Etcd client 计数器快照。
 * @details 通过 `galay_etcd_client_stats` 按值复制；结构中的计数值不拥有任何外部
 * 资源，可跨线程读取副本。
 * @note client 本身未声明跨线程同步，统计值只反映调用点可见的内部状态。
 */
typedef struct galay_etcd_client_stats_t {
    uint64_t requests;                   ///< 已发起 HTTP 请求数。
    uint64_t request_failures;           ///< 请求发送、读取或解析失败次数。
    uint64_t retries;                    ///< 重试次数。
    uint64_t endpoint_switches;          ///< endpoint 切换次数。
    uint64_t auth_refreshes;             ///< 认证刷新次数。
    uint64_t watch_reconnects;           ///< watch 重连次数。
    uint64_t watch_compactions;          ///< watch compaction 次数。
    uint64_t lease_keepalive_successes;  ///< lease keepalive 成功次数。
    uint64_t lease_keepalive_failures;   ///< lease keepalive 失败次数。
} galay_etcd_client_stats_t;

/**
 * @brief Etcd 配置构建器 handle。
 * @details 由 `galay_etcd_config_builder_create` 创建，调用方拥有，使用
 * `galay_etcd_config_builder_destroy` 释放。传给 client create 后配置会被复制。
 */
typedef struct galay_etcd_config_builder_t galay_etcd_config_builder_t;

/**
 * @brief Etcd client handle。
 * @details 由 `galay_etcd_client_create` 创建并拥有内部 TCP socket；调用方用
 * `galay_etcd_client_destroy` 释放。I/O 调用会使用 C coroutine TCP 接口，应在同一
 * C runtime/coroutine 调度上下文内顺序调用。
 */
typedef struct galay_etcd_client_t galay_etcd_client_t;

/**
 * @brief Etcd get 结果 handle。
 * @details 由 get/pipeline 或 empty create 产生；item accessor 返回的 key/value
 * buffer 借用该结果内部存储，直到 destroy 或所属 pipeline result destroy 前有效。
 */
typedef struct galay_etcd_get_result_t galay_etcd_get_result_t;

/**
 * @brief Etcd pipeline 请求缓存 handle。
 * @details add 函数会复制 key/value 到 pipeline 内部；execute 时按添加顺序执行。
 */
typedef struct galay_etcd_pipeline_t galay_etcd_pipeline_t;

/**
 * @brief Etcd pipeline 执行结果 handle。
 * @details execute 成功后由调用方拥有，必须用 `galay_etcd_pipeline_result_destroy`
 * 释放；内部 get result accessor 返回借用指针，不得单独 destroy。
 */
typedef struct galay_etcd_pipeline_result_t galay_etcd_pipeline_result_t;

/**
 * @brief Etcd watch handle。
 * @details watch 借用创建时传入的 client；client 必须比 watch 存活更久，且 close 或
 * destroy client 后不得继续 next。
 */
typedef struct galay_etcd_watch_t galay_etcd_watch_t;

/**
 * @brief Etcd watch event handle。
 * @details `galay_etcd_watch_next` 成功后由调用方拥有，必须用
 * `galay_etcd_watch_event_destroy` 释放；key/value 视图借用 event 内部存储。
 */
typedef struct galay_etcd_watch_event_t galay_etcd_watch_event_t;

/**
 * @brief 返回 Etcd 错误码静态字符串。
 * @param code Etcd 错误码。
 * @return 永久有效的 NUL 结尾静态字符串；未知值返回 "unknown"。
 */
const char* galay_etcd_error_string(galay_etcd_error_code_t code);

/**
 * @brief 将 Etcd 错误码映射为通用 `galay_status_t`。
 * @param code Etcd 错误码。
 * @return 对应通用状态码；未知值返回 `GALAY_INTERNAL_ERROR`。
 */
galay_status_t galay_etcd_error_status(galay_etcd_error_code_t code);

/**
 * @brief 创建 Etcd 配置构建器。
 * @param out 成功时返回 builder；调用方用 `galay_etcd_config_builder_destroy` 释放。
 * @return `GALAY_OK`、`GALAY_INVALID_ARGUMENT` 或 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_etcd_config_builder_create(galay_etcd_config_builder_t** out);

/**
 * @brief 销毁 Etcd 配置构建器。
 * @param builder 可为 NULL；销毁后不得再传给 client create。
 */
void galay_etcd_config_builder_destroy(galay_etcd_config_builder_t* builder);

/**
 * @brief 设置 Etcd HTTP endpoint。
 * @param builder 配置构建器。
 * @param endpoint NUL 结尾 endpoint；当前实现要求 `http://host:port`，会复制字符串。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`，成功返回 `GALAY_OK`。
 * @note endpoint 在 connect 时解析；无效 endpoint 会通过 `GALAY_ETCD_ERROR_INVALID_ENDPOINT`
 * 返回。
 */
galay_status_t galay_etcd_config_builder_set_endpoint(galay_etcd_config_builder_t* builder,
                                                      const char* endpoint);

/**
 * @brief 设置 endpoint 选择策略。
 * @param builder 配置构建器。
 * @param policy `galay_etcd_endpoint_policy_t` 枚举值。
 * @return 枚举或 builder 无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_etcd_config_builder_set_endpoint_policy(
    galay_etcd_config_builder_t* builder,
    galay_etcd_endpoint_policy_t policy);

/**
 * @brief 创建 Etcd client。
 * @param builder 可为 NULL；NULL 使用 `http://127.0.0.1:2379` 和默认策略。
 * @param out 成功时返回 client；调用方用 `galay_etcd_client_destroy` 释放。
 * @return `GALAY_OK`、`GALAY_INVALID_ARGUMENT` 或 `GALAY_OUT_OF_MEMORY`。
 * @note builder 中的配置会被复制，client 生命周期不依赖 builder。
 */
galay_status_t galay_etcd_client_create(const galay_etcd_config_builder_t* builder,
                                        galay_etcd_client_t** out);

/**
 * @brief 销毁 Etcd client 及其拥有的 socket。
 * @param client 可为 NULL；销毁后所有借用该 client 的 watch 均失效。
 * @note 本函数不抛异常；若底层 socket destroy 失败，只会清理 C handle 状态。
 */
void galay_etcd_client_destroy(galay_etcd_client_t* client);

/**
 * @brief 连接配置中的 Etcd endpoint。
 * @param client Etcd client。
 * @param code 可选输出 Etcd 细分错误码；成功写入 `GALAY_ETCD_ERROR_SUCCESS`。
 * @return `GALAY_OK` 表示已连接；参数、endpoint 或 I/O 错误通过返回值显式传播。
 * @note 使用 C coroutine TCP connect；应在 galay C runtime/coroutine 上下文内调用。
 * 当前实现使用内部默认超时，不提供独立 timeout 参数。
 */
galay_status_t galay_etcd_client_connect(galay_etcd_client_t* client,
                                         galay_etcd_error_code_t* code);

/**
 * @brief 关闭 Etcd client socket。
 * @param client 已连接或持有 socket 的 client。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 成功返回 `GALAY_OK`；未连接、关闭失败或销毁 socket 失败通过返回值传播。
 * @note close 会清理内部接收缓存并标记 client 未连接；不销毁 client handle。
 */
galay_status_t galay_etcd_client_close(galay_etcd_client_t* client,
                                       galay_etcd_error_code_t* code);

/**
 * @brief 写入 key/value。
 * @param client 已连接 Etcd client。
 * @param key NUL 结尾 key，不能为空。
 * @param value value buffer；`value_len == 0` 时可为 NULL。
 * @param value_len value 字节数，不要求 NUL 结尾。
 * @param code 可选输出 Etcd 细分错误码。
 * @return `GALAY_OK` 表示 Etcd HTTP put 成功；未连接、参数、I/O 或协议错误显式返回。
 */
galay_status_t galay_etcd_client_put(galay_etcd_client_t* client, const char* key,
                                     const char* value, size_t value_len,
                                     galay_etcd_error_code_t* code);

/**
 * @brief 读取 key 或前缀范围。
 * @param client 已连接 Etcd client。
 * @param key NUL 结尾 key，不能为空。
 * @param prefix `GALAY_TRUE` 表示读取 key 前缀范围。
 * @param limit 最大返回条数；0 表示不限制，负数非法。
 * @param result 成功时返回 get result，调用方用 `galay_etcd_get_result_destroy` 释放。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 成功返回 `GALAY_OK`；失败时 `*result` 被置为 NULL。
 * @note item accessor 返回的 key/value 指针借用 result 内部存储。
 */
galay_status_t galay_etcd_client_get(galay_etcd_client_t* client, const char* key,
                                     galay_bool_t prefix, int64_t limit,
                                     galay_etcd_get_result_t** result,
                                     galay_etcd_error_code_t* code);

/**
 * @brief 删除 key 或前缀范围。
 * @param client 已连接 Etcd client。
 * @param key NUL 结尾 key，不能为空。
 * @param prefix `GALAY_TRUE` 表示删除 key 前缀范围。
 * @param deleted_count 可选输出删除数量；失败或未提供时不要求写入。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 成功返回 `GALAY_OK`；响应缺少 deleted 字段返回 `GALAY_PROTOCOL_ERROR`。
 */
galay_status_t galay_etcd_client_delete(galay_etcd_client_t* client, const char* key,
                                        galay_bool_t prefix, int64_t* deleted_count,
                                        galay_etcd_error_code_t* code);

/**
 * @brief 创建空 get result。
 * @param out 成功时返回空结果，调用方负责 destroy。
 * @return `GALAY_OK`、`GALAY_INVALID_ARGUMENT` 或 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_etcd_get_result_create_empty(galay_etcd_get_result_t** out);

/**
 * @brief 销毁 get result。
 * @param result 可为 NULL；销毁后其 item key/value 借用指针全部失效。
 */
void galay_etcd_get_result_destroy(galay_etcd_get_result_t* result);

/**
 * @brief 获取 get result 条目数。
 * @param result get result handle。
 * @param count 输出条目数。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_etcd_get_result_count(const galay_etcd_get_result_t* result, size_t* count);

/**
 * @brief 获取 get result 中一个 key/value 条目。
 * @param result get result handle。
 * @param index 条目索引。
 * @param key 输出借用 key buffer。
 * @param key_len 输出 key 字节数。
 * @param value 输出借用 value buffer。
 * @param value_len 输出 value 字节数。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`。
 * @note key/value 不追加 NUL，且只在 result 销毁前有效；调用方不得释放。
 */
galay_status_t galay_etcd_get_result_item(const galay_etcd_get_result_t* result, size_t index,
                                          const char** key, size_t* key_len,
                                          const char** value, size_t* value_len);

/**
 * @brief 创建 lease。
 * @param client 已连接 Etcd client。
 * @param ttl_seconds TTL 秒数，必须大于 0。
 * @param lease_id 成功时输出 lease ID。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 成功返回 `GALAY_OK`；响应缺少 ID 返回 `GALAY_PROTOCOL_ERROR`。
 */
galay_status_t galay_etcd_client_lease_grant(galay_etcd_client_t* client,
                                             int64_t ttl_seconds,
                                             int64_t* lease_id,
                                             galay_etcd_error_code_t* code);

/**
 * @brief 刷新 lease。
 * @param client 已连接 Etcd client。
 * @param lease_id 需要刷新 lease ID，必须大于 0。
 * @param refreshed_lease_id 成功时输出 Etcd 返回的 lease ID。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 成功返回 `GALAY_OK`；失败会更新 keepalive 统计。
 */
galay_status_t galay_etcd_client_lease_keepalive(galay_etcd_client_t* client,
                                                 int64_t lease_id,
                                                 int64_t* refreshed_lease_id,
                                                 galay_etcd_error_code_t* code);

/**
 * @brief 撤销 lease。
 * @param client 已连接 Etcd client。
 * @param lease_id lease ID，必须大于 0。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 成功返回 `GALAY_OK`；参数、未连接或 I/O 错误通过返回值传播。
 */
galay_status_t galay_etcd_client_lease_revoke(galay_etcd_client_t* client,
                                              int64_t lease_id,
                                              galay_etcd_error_code_t* code);

/**
 * @brief 创建 pipeline 请求缓存。
 * @param out 成功时返回 pipeline；调用方用 `galay_etcd_pipeline_destroy` 释放。
 * @return `GALAY_OK`、`GALAY_INVALID_ARGUMENT` 或 `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_etcd_pipeline_create(galay_etcd_pipeline_t** out);

/**
 * @brief 销毁 pipeline 请求缓存。
 * @param pipeline 可为 NULL；销毁后已添加操作不可再执行。
 */
void galay_etcd_pipeline_destroy(galay_etcd_pipeline_t* pipeline);

/**
 * @brief 向 pipeline 追加 put 操作。
 * @param pipeline pipeline handle。
 * @param key NUL 结尾 key，不能为空。
 * @param value value buffer；`value_len == 0` 时可为 NULL。
 * @param value_len value 字节数。
 * @param lease_id 可选 lease ID；0 表示不绑定 lease，负数非法。
 * @return 成功返回 `GALAY_OK`；key/value 会被复制到 pipeline。
 */
galay_status_t galay_etcd_pipeline_add_put(galay_etcd_pipeline_t* pipeline,
                                           const char* key,
                                           const char* value,
                                           size_t value_len,
                                           int64_t lease_id);

/**
 * @brief 向 pipeline 追加 get 操作。
 * @param pipeline pipeline handle。
 * @param key NUL 结尾 key，不能为空。
 * @param prefix 是否按前缀读取。
 * @param limit 最大返回条数；0 表示不限制，负数非法。
 * @return 成功返回 `GALAY_OK`；key 会被复制到 pipeline。
 */
galay_status_t galay_etcd_pipeline_add_get(galay_etcd_pipeline_t* pipeline,
                                           const char* key,
                                           galay_bool_t prefix,
                                           int64_t limit);

/**
 * @brief 向 pipeline 追加 delete 操作。
 * @param pipeline pipeline handle。
 * @param key NUL 结尾 key，不能为空。
 * @param prefix 是否按前缀删除。
 * @return 成功返回 `GALAY_OK`；key 会被复制到 pipeline。
 */
galay_status_t galay_etcd_pipeline_add_delete(galay_etcd_pipeline_t* pipeline,
                                              const char* key,
                                              galay_bool_t prefix);

/**
 * @brief 按添加顺序执行 pipeline。
 * @param client 已连接 Etcd client。
 * @param pipeline 非空 pipeline。
 * @param result 成功时返回 pipeline result；调用方必须 destroy。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 全部操作成功才返回 `GALAY_OK`；任一操作失败会停止并释放中间结果。
 * @note 当前实现逐条执行，不保证 Etcd 事务原子性；result 中 get item 为借用视图。
 */
galay_status_t galay_etcd_client_pipeline_execute(galay_etcd_client_t* client,
                                                  const galay_etcd_pipeline_t* pipeline,
                                                  galay_etcd_pipeline_result_t** result,
                                                  galay_etcd_error_code_t* code);

/**
 * @brief 销毁 pipeline result。
 * @param result 可为 NULL；销毁后其内部 get result 借用指针全部失效。
 */
void galay_etcd_pipeline_result_destroy(galay_etcd_pipeline_result_t* result);

/**
 * @brief 获取 pipeline result 条目数。
 * @param result pipeline result handle。
 * @param count 输出条目数。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_etcd_pipeline_result_count(const galay_etcd_pipeline_result_t* result,
                                                size_t* count);

/**
 * @brief 获取 pipeline result 中一个条目的操作类型。
 * @param result pipeline result handle。
 * @param index 条目索引。
 * @param type 输出操作类型。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`。
 */
galay_status_t galay_etcd_pipeline_result_item_type(
    const galay_etcd_pipeline_result_t* result,
    size_t index,
    galay_etcd_pipeline_op_type_t* type);

/**
 * @brief 获取 pipeline get 条目的结果视图。
 * @param result pipeline result handle。
 * @param index 条目索引，必须对应 `GALAY_ETCD_PIPELINE_GET`。
 * @param get_result 输出借用 get result 指针。
 * @return 成功返回 `GALAY_OK`；越界、类型不匹配或参数无效返回错误。
 * @note 返回指针由 pipeline result 拥有，不得调用 `galay_etcd_get_result_destroy`。
 */
galay_status_t galay_etcd_pipeline_result_item_get_result(
    const galay_etcd_pipeline_result_t* result,
    size_t index,
    const galay_etcd_get_result_t** get_result);

/**
 * @brief 获取 pipeline delete 条目的删除数量。
 * @param result pipeline result handle。
 * @param index 条目索引，必须对应 `GALAY_ETCD_PIPELINE_DELETE`。
 * @param deleted_count 输出删除数量。
 * @return 成功返回 `GALAY_OK`；越界、类型不匹配或参数无效返回错误。
 */
galay_status_t galay_etcd_pipeline_result_item_deleted_count(
    const galay_etcd_pipeline_result_t* result,
    size_t index,
    int64_t* deleted_count);

/**
 * @brief 创建 watch handle。
 * @param client 已连接 Etcd client；watch 借用该 client。
 * @param key NUL 结尾 key，不能为空。
 * @param prefix 是否 watch 前缀范围。
 * @param watch 成功时返回 watch；调用方用 `galay_etcd_watch_destroy` 释放。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 成功返回 `GALAY_OK`；未连接或参数错误显式返回。
 * @note watch 不拥有 client；client close/destroy 前应先停止使用并销毁 watch。
 */
galay_status_t galay_etcd_watch_create(galay_etcd_client_t* client,
                                       const char* key,
                                       galay_bool_t prefix,
                                       galay_etcd_watch_t** watch,
                                       galay_etcd_error_code_t* code);

/**
 * @brief 销毁 watch handle。
 * @param watch 可为 NULL；不会隐式关闭或销毁 client。
 */
void galay_etcd_watch_destroy(galay_etcd_watch_t* watch);

/**
 * @brief 读取下一个 watch event。
 * @param watch watch handle。
 * @param event 成功时返回 event；调用方用 `galay_etcd_watch_event_destroy` 释放。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 成功返回 `GALAY_OK`；cancel 后返回 `GALAY_IO_ERROR` 并设置 cancelled。
 * @note 当前实现每次调用发送一次 watch HTTP 请求；event key/value buffer 借用 event。
 */
galay_status_t galay_etcd_watch_next(galay_etcd_watch_t* watch,
                                     galay_etcd_watch_event_t** event,
                                     galay_etcd_error_code_t* code);

/**
 * @brief 取消 watch。
 * @param watch watch handle。
 * @param code 可选输出 Etcd 细分错误码。
 * @return 成功标记取消返回 `GALAY_OK`；NULL watch 返回 `GALAY_INVALID_ARGUMENT`。
 * @note cancel 只更新本地状态，不销毁 watch，也不关闭 client。
 */
galay_status_t galay_etcd_watch_cancel(galay_etcd_watch_t* watch,
                                       galay_etcd_error_code_t* code);

/**
 * @brief 销毁 watch event。
 * @param event 可为 NULL；销毁后 key/value 借用指针失效。
 */
void galay_etcd_watch_event_destroy(galay_etcd_watch_event_t* event);

/**
 * @brief 获取 watch event 的 watch ID。
 * @param event watch event handle。
 * @param watch_id 输出 watch ID；响应缺省时为 0。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_etcd_watch_event_watch_id(const galay_etcd_watch_event_t* event,
                                               int64_t* watch_id);

/**
 * @brief 获取 watch event 类型。
 * @param event watch event handle。
 * @param type 输出事件类型。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_etcd_watch_event_type(const galay_etcd_watch_event_t* event,
                                           galay_etcd_watch_event_type_t* type);

/**
 * @brief 获取 watch event key/value。
 * @param event watch event handle。
 * @param key 输出借用 key buffer。
 * @param key_len 输出 key 字节数。
 * @param value 输出借用 value buffer。
 * @param value_len 输出 value 字节数。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note key/value 不追加 NUL，只在 event destroy 前有效；调用方不得释放。
 */
galay_status_t galay_etcd_watch_event_key_value(const galay_etcd_watch_event_t* event,
                                                const char** key,
                                                size_t* key_len,
                                                const char** value,
                                                size_t* value_len);

/**
 * @brief 获取 client 统计快照。
 * @param client Etcd client。
 * @param stats 输出按值复制的统计快照。
 * @return 参数无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note client 无跨线程同步契约；应在同一调用上下文读取统计。
 */
galay_status_t galay_etcd_client_stats(const galay_etcd_client_t* client,
                                       galay_etcd_client_stats_t* stats);

#ifdef __cplusplus
}
#endif

#endif
