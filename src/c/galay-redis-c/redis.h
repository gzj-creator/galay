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
    GALAY_REDIS_RESP_ARRAY = 4
} galay_redis_resp_type_t;

typedef struct galay_redis_command_builder_t galay_redis_command_builder_t;
typedef struct galay_redis_reply_t galay_redis_reply_t;
typedef struct galay_redis_client_t galay_redis_client_t;

typedef struct galay_redis_client_config_t {
    const char* host;
    uint16_t port;
    const char* username;
    const char* password;
    int db_index;
    int resp_version;
    int connect_timeout_ms;
} galay_redis_client_config_t;

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
galay_redis_resp_type_t galay_redis_reply_type(const galay_redis_reply_t* reply);
galay_status_t galay_redis_reply_string(const galay_redis_reply_t* reply, const char** value,
                                        size_t* value_len);
galay_status_t galay_redis_reply_array_size(const galay_redis_reply_t* reply, size_t* size);
galay_status_t galay_redis_client_create(const galay_redis_client_config_t* config,
                                         galay_redis_client_t** out);
void galay_redis_client_destroy(galay_redis_client_t* client);
galay_status_t galay_redis_client_disconnect(galay_redis_client_t* client);
galay_status_t galay_redis_client_command(galay_redis_client_t* client, const char* command,
                                          const char* const* args, const size_t* arg_lens,
                                          size_t arg_count, galay_redis_reply_t** reply);

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
