#ifndef GALAY_C_REDIS_REDIS_H
#define GALAY_C_REDIS_REDIS_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_redis_resp_type_t {
    GALAY_REDIS_RESP_SIMPLE_STRING = 0,
    GALAY_REDIS_RESP_ERROR = 1,
    GALAY_REDIS_RESP_INTEGER = 2,
    GALAY_REDIS_RESP_BULK_STRING = 3,
    GALAY_REDIS_RESP_ARRAY = 4,
    GALAY_REDIS_RESP_NIL = 5,
    GALAY_REDIS_RESP_DOUBLE = 6,
    GALAY_REDIS_RESP_BOOLEAN = 7,
    GALAY_REDIS_RESP_BLOB_ERROR = 8,
    GALAY_REDIS_RESP_VERBATIM_STRING = 9,
    GALAY_REDIS_RESP_BIG_NUMBER = 10,
    GALAY_REDIS_RESP_MAP = 11,
    GALAY_REDIS_RESP_SET = 12,
    GALAY_REDIS_RESP_PUSH = 13
} galay_redis_resp_type_t;

typedef enum galay_redis_redirect_type_t {
    GALAY_REDIS_REDIRECT_NONE = 0,
    GALAY_REDIS_REDIRECT_MOVED = 1,
    GALAY_REDIS_REDIRECT_ASK = 2
} galay_redis_redirect_type_t;

typedef struct galay_redis_command_builder_t galay_redis_command_builder_t;
typedef struct galay_redis_reply_t galay_redis_reply_t;
typedef struct galay_redis_client_t galay_redis_client_t;
typedef struct galay_redis_pipeline_t galay_redis_pipeline_t;
typedef struct galay_redis_pool_t galay_redis_pool_t;
typedef struct galay_redis_pool_lease_t galay_redis_pool_lease_t;
typedef struct galay_redis_cluster_t galay_redis_cluster_t;

typedef struct galay_redis_client_config_t {
    const char* host;
    uint16_t port;
    const char* username;
    const char* password;
    int db_index;
    int resp_version;
    int connect_timeout_ms;
} galay_redis_client_config_t;

typedef struct galay_redis_pool_config_t {
    galay_redis_client_config_t client;
    size_t min_connections;
    size_t max_connections;
    size_t initial_connections;
} galay_redis_pool_config_t;

typedef struct galay_redis_cluster_node_config_t {
    const char* host;
    uint16_t port;
    uint16_t slot_start;
    uint16_t slot_end;
} galay_redis_cluster_node_config_t;

typedef struct galay_redis_cluster_route_t {
    uint16_t slot;
    size_t node_index;
    const char* host;
    uint16_t port;
    galay_redis_redirect_type_t redirect_type;
} galay_redis_cluster_route_t;

const char* galay_redis_get_error(galay_status_t status);
galay_status_t galay_redis_command_builder_create(galay_redis_command_builder_t** out);
void galay_redis_command_builder_destroy(galay_redis_command_builder_t* builder);
galay_status_t galay_redis_command_builder_build(galay_redis_command_builder_t* builder,
                                                 const char* command,
                                                 const char* const* args,
                                                 const size_t* arg_lens,
                                                 size_t arg_count,
                                                 const char** encoded,
                                                 size_t* encoded_len);
galay_status_t galay_redis_parse_reply(const char* data, size_t data_len,
                                       galay_redis_reply_t** out, size_t* consumed);
void galay_redis_reply_destroy(galay_redis_reply_t* reply);
void galay_redis_reply_free(galay_redis_reply_t* reply);
galay_redis_resp_type_t galay_redis_reply_type(const galay_redis_reply_t* reply);
galay_status_t galay_redis_reply_string(const galay_redis_reply_t* reply, const char** value,
                                        size_t* value_len);
galay_status_t galay_redis_reply_integer(const galay_redis_reply_t* reply, int64_t* value);
galay_status_t galay_redis_reply_double(const galay_redis_reply_t* reply, double* value);
galay_status_t galay_redis_reply_boolean(const galay_redis_reply_t* reply, galay_bool_t* value);
galay_status_t galay_redis_reply_array_size(const galay_redis_reply_t* reply, size_t* size);
galay_status_t galay_redis_reply_array_at(const galay_redis_reply_t* reply, size_t index,
                                          const galay_redis_reply_t** out);
galay_status_t galay_redis_reply_map_size(const galay_redis_reply_t* reply, size_t* size);
galay_status_t galay_redis_reply_map_at(const galay_redis_reply_t* reply, size_t index,
                                        const galay_redis_reply_t** key,
                                        const galay_redis_reply_t** value);
galay_status_t galay_redis_client_create(const galay_redis_client_config_t* config,
                                         galay_redis_client_t** out);
void galay_redis_client_destroy(galay_redis_client_t* client);
galay_status_t galay_redis_client_disconnect(galay_redis_client_t* client);
galay_status_t galay_redis_client_command(galay_redis_client_t* client, const char* command,
                                          const char* const* args, const size_t* arg_lens,
                                          size_t arg_count, galay_redis_reply_t** reply);

/**
 * @brief 创建 Redis pipeline 命令缓存。
 * @param out 成功时返回 pipeline，调用方负责用 `galay_redis_pipeline_destroy` 释放。
 * @return `GALAY_OK` 表示创建成功；无效参数或内存不足通过 `galay_status_t` 返回。
 */
galay_status_t galay_redis_pipeline_create(galay_redis_pipeline_t** out);

/**
 * @brief 销毁 Redis pipeline 命令缓存。
 * @param pipeline 可为 NULL；销毁后其中缓存的命令不可再使用。
 */
void galay_redis_pipeline_destroy(galay_redis_pipeline_t* pipeline);

/**
 * @brief 向 pipeline 追加一条 Redis 命令。
 * @param pipeline 由 `galay_redis_pipeline_create` 创建的 pipeline。
 * @param command Redis 命令名，例如 "PING"。
 * @param args 命令参数数组；`arg_count` 为 0 时可为 NULL。
 * @param arg_lens 每个参数长度；为 NULL 时按 C 字符串长度计算。
 * @param arg_count 参数数量。
 * @return `GALAY_OK` 表示命令已编码并缓存；无效参数或编码失败通过返回值传播。
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
 */
void galay_redis_pipeline_replies_destroy(galay_redis_reply_t** replies, size_t reply_count);

/**
 * @brief 在当前 C coroutine 内异步连接 Redis standalone 节点。
 * @param client 由 `galay_redis_client_create` 创建的 client。
 * @param timeout_ms 负数无限等待，0 直接超时，正数为毫秒超时。
 * @return `C_IOResultOk` 表示连接成功；参数无效、未在 C coroutine 内调用或超时通过
 *         `C_IOResult` 返回。该函数会挂起当前 C coroutine，不阻塞线程。
 */
C_IOResult galay_redis_client_connect(galay_redis_client_t* client, int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 内发送一条 Redis 命令并等待一个 RESP reply。
 * @param client 已连接的 Redis client。
 * @param command Redis 命令名，例如 "PING"。
 * @param args 命令参数数组；`arg_count` 为 0 时可为 NULL。
 * @param arg_lens 每个参数长度；为 NULL 时按 C 字符串长度计算。
 * @param arg_count 参数数量。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param reply 成功时返回 reply，调用方负责用 `galay_redis_reply_destroy` 释放。
 * @return `C_IOResultOk` 表示命令发送和 reply 解析成功；错误通过返回值显式传播。
 * @note 当前最小实现复用 C RESP parser，支持其已覆盖的 reply 类型。
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
 * @param client 已连接的 Redis client。
 * @param username Redis 6 ACL 用户名；为 NULL 或空字符串时发送旧式 `AUTH password`。
 * @param password 认证密码，不能为 NULL。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @return `C_IOResultOk` 表示认证成功；无效参数、网络错误或非 OK reply 通过返回值传播。
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
 */
C_IOResult galay_redis_client_select(galay_redis_client_t* client,
                                     int db_index,
                                     int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 内发送 pipeline 中的所有命令并保留每个 reply。
 * @param client 已连接的 Redis client。
 * @param pipeline 由 `galay_redis_pipeline_create` 创建并添加命令的 pipeline。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param replies 成功时返回 reply 指针数组，调用方负责用
 *        `galay_redis_pipeline_replies_destroy` 释放。
 * @param reply_count 成功时返回 reply 数量。
 * @return `C_IOResultOk` 表示所有命令写入且读到同等数量 reply；错误通过返回值传播。
 * @note 当前最小实现按添加顺序发送命令，并按顺序读取 reply，不额外提供乱序匹配。
 */
C_IOResult galay_redis_client_pipeline_async(galay_redis_client_t* client,
                                             const galay_redis_pipeline_t* pipeline,
                                             int64_t timeout_ms,
                                             galay_redis_reply_t*** replies,
                                             size_t* reply_count);

/**
 * @brief 创建 Redis C 连接池。
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
 */
void galay_redis_pool_destroy(galay_redis_pool_t* pool);

/**
 * @brief 在当前 C coroutine 内获取一个 Redis 连接 lease。
 * @param pool 由 `galay_redis_pool_create` 创建的连接池。
 * @param timeout_ms 新建连接时使用的连接超时；连接池已满时返回 `C_IOResultTimeout`。
 * @param lease 成功时返回 lease，必须用 `galay_redis_pool_release` 归还。
 * @return 成功返回 `C_IOResultOk`；参数错误、连接失败或池耗尽通过返回值传播。
 */
C_IOResult galay_redis_pool_acquire(galay_redis_pool_t* pool,
                                    int64_t timeout_ms,
                                    galay_redis_pool_lease_t** lease);

/**
 * @brief 归还一个 Redis pool lease。
 * @param pool lease 所属 pool。
 * @param lease 由 `galay_redis_pool_acquire` 返回的 lease。
 * @return `GALAY_OK` 表示归还成功；重复归还或 pool 不匹配返回错误。
 */
galay_status_t galay_redis_pool_release(galay_redis_pool_t* pool,
                                        galay_redis_pool_lease_t* lease);

/**
 * @brief 获取 lease 中借用的 client。
 * @return 返回的 client 由 pool 拥有，生命周期到 lease release 为止；调用方不得 destroy。
 */
galay_redis_client_t* galay_redis_pool_lease_client(galay_redis_pool_lease_t* lease);

/**
 * @brief 创建最小 Redis Cluster 路由表。
 * @param out 成功时返回 cluster，调用方负责用 `galay_redis_cluster_destroy` 释放。
 */
galay_status_t galay_redis_cluster_create(galay_redis_cluster_t** out);

/**
 * @brief 销毁 Redis Cluster 路由表。
 * @param cluster 可为 NULL。
 */
void galay_redis_cluster_destroy(galay_redis_cluster_t* cluster);

/**
 * @brief 添加一个节点及其 slot 覆盖范围。
 * @param node host/port 和闭区间 slot range；host 字符串会被复制到 cluster。
 */
galay_status_t galay_redis_cluster_add_node(galay_redis_cluster_t* cluster,
                                            const galay_redis_cluster_node_config_t* node);

/**
 * @brief 计算 Redis Cluster key slot。
 * @param key key 字节序列；支持 `{tag}` hash tag。
 * @param key_len key 字节长度。
 * @param slot 成功时返回 0-16383。
 */
galay_status_t galay_redis_cluster_key_slot(const char* key, size_t key_len, uint16_t* slot);

/**
 * @brief 按 slot 查询当前路由节点。
 * @param route 成功时返回借用 host 指针；指针在下一次 cluster 修改前有效。
 */
galay_status_t galay_redis_cluster_route_slot(const galay_redis_cluster_t* cluster,
                                              uint16_t slot,
                                              galay_redis_cluster_route_t* route);

/**
 * @brief 按 key 计算 slot 并查询当前路由节点。
 */
galay_status_t galay_redis_cluster_route_key(const galay_redis_cluster_t* cluster,
                                             const char* key,
                                             size_t key_len,
                                             galay_redis_cluster_route_t* route);

/**
 * @brief 解析 `-MOVED` 或 `-ASK` reply 并返回目标路由。
 * @details MOVED 会更新 slot 路由缓存；ASK 只返回临时目标，不修改缓存。
 */
galay_status_t galay_redis_cluster_apply_redirect(galay_redis_cluster_t* cluster,
                                                  const galay_redis_reply_t* reply,
                                                  galay_redis_cluster_route_t* route);

/**
 * @brief 在当前 C coroutine 内关闭 Redis TCP 连接并释放内部 socket。
 * @param client 已连接或持有 socket 的 Redis client。
 * @param timeout_ms 关闭操作超时，语义同 kernel TCP close。
 * @return `C_IOResultOk` 表示关闭并清理成功；失败通过返回值传播。
 */
C_IOResult galay_redis_client_close(galay_redis_client_t* client, int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
