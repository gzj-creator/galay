/**
 * @file redis.h
 * @brief galay-redis C ABI 封装。
 *
 * @details 该头文件只暴露 C 兼容的 opaque handle、值类型配置和显式错误码。
 *          返回的 const char* 指针由对应 handle 持有，在下一次修改该 handle
 *          或 destroy 前有效，调用方不得释放。
 */

#ifndef GALAY_C_REDIS_REDIS_H
#define GALAY_C_REDIS_REDIS_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

typedef struct galay_redis_command_builder galay_redis_command_builder_t;
typedef struct galay_redis_reply galay_redis_reply_t;
typedef struct galay_redis_client galay_redis_client_t;

typedef enum galay_redis_resp_type {
    GALAY_REDIS_RESP_SIMPLE_STRING = 0,
    GALAY_REDIS_RESP_ERROR = 1,
    GALAY_REDIS_RESP_INTEGER = 2,
    GALAY_REDIS_RESP_BULK_STRING = 3,
    GALAY_REDIS_RESP_ARRAY = 4,
    GALAY_REDIS_RESP_NULL = 5,
    GALAY_REDIS_RESP_DOUBLE = 6,
    GALAY_REDIS_RESP_BOOLEAN = 7,
    GALAY_REDIS_RESP_MAP = 8,
    GALAY_REDIS_RESP_SET = 9,
    GALAY_REDIS_RESP_PUSH = 10,
    GALAY_REDIS_RESP_UNKNOWN = 255
} galay_redis_resp_type_t;

typedef struct galay_redis_client_config {
    const char* host;              ///< NULL 使用 127.0.0.1。
    int32_t port;                  ///< 0 使用 6379；有效范围 1..65535。
    const char* username;          ///< 可为 NULL；非空时 password 也必须非空。
    const char* password;          ///< 可为 NULL；非空时连接阶段会发送 AUTH/HELLO AUTH。
    int32_t db_index;              ///< Redis DB index，必须大于等于 0。
    int32_t resp_version;          ///< 0 使用 RESP2；显式值仅支持 2 或 3。
    uint32_t connect_timeout_ms;   ///< 0 使用 5000ms。
} galay_redis_client_config_t;

/**
 * @brief 创建 RESP 命令构建器。
 */
GALAY_C_API galay_status_t galay_redis_command_builder_create(galay_redis_command_builder_t** out);

/**
 * @brief 销毁 RESP 命令构建器；NULL 安全。
 */
GALAY_C_API void galay_redis_command_builder_destroy(galay_redis_command_builder_t* builder);

/**
 * @brief 构建 Redis 命令 RESP bulk array。
 * @param builder 命令构建器。
 * @param command 命令名，必须非 NULL 且非空。
 * @param args 参数数组；arg_count 为 0 时可为 NULL。
 * @param arg_lens 参数长度数组；为 NULL 时参数按 NUL 结尾字符串处理。
 * @param arg_count 参数数量。
 * @param out_data 输出编码数据，由 builder 持有。
 * @param out_len 输出编码长度。
 */
GALAY_C_API galay_status_t galay_redis_command_builder_build(galay_redis_command_builder_t* builder,
                                                             const char* command,
                                                             const char* const* args,
                                                             const size_t* arg_lens,
                                                             size_t arg_count,
                                                             const char** out_data,
                                                             size_t* out_len);

/**
 * @brief 解析单个 RESP reply。
 * @param data 输入 RESP 字节；data_len 为 0 时可为 NULL。
 * @param data_len 输入长度。
 * @param out_reply 输出 reply，由调用方 destroy。
 * @param consumed 成功解析时输出消费字节数。
 */
GALAY_C_API galay_status_t galay_redis_parse_reply(const void* data,
                                                   size_t data_len,
                                                   galay_redis_reply_t** out_reply,
                                                   size_t* consumed);

/**
 * @brief 销毁 reply；NULL 安全。
 */
GALAY_C_API void galay_redis_reply_destroy(galay_redis_reply_t* reply);

/**
 * @brief 获取 reply RESP 类型；NULL 返回 GALAY_REDIS_RESP_UNKNOWN。
 */
GALAY_C_API galay_redis_resp_type_t galay_redis_reply_type(const galay_redis_reply_t* reply);

GALAY_C_API galay_status_t galay_redis_reply_string(const galay_redis_reply_t* reply,
                                                    const char** value,
                                                    size_t* value_len);
GALAY_C_API galay_status_t galay_redis_reply_integer(const galay_redis_reply_t* reply,
                                                     int64_t* value);
GALAY_C_API galay_status_t galay_redis_reply_double(const galay_redis_reply_t* reply,
                                                    double* value);
GALAY_C_API galay_status_t galay_redis_reply_boolean(const galay_redis_reply_t* reply,
                                                     galay_bool_t* value);
GALAY_C_API galay_status_t galay_redis_reply_array_size(const galay_redis_reply_t* reply,
                                                        size_t* size);

/**
 * @brief 获取数组元素只读 handle；元素由父 reply 持有，调用方不得 destroy。
 */
GALAY_C_API galay_status_t galay_redis_reply_array_get(const galay_redis_reply_t* reply,
                                                       size_t index,
                                                       const galay_redis_reply_t** out);

GALAY_C_API galay_status_t galay_redis_client_create(const galay_redis_client_config_t* config,
                                                     galay_redis_client_t** out);
GALAY_C_API void galay_redis_client_destroy(galay_redis_client_t* client);
GALAY_C_API galay_status_t galay_redis_client_connect(galay_redis_client_t* client);
GALAY_C_API galay_status_t galay_redis_client_disconnect(galay_redis_client_t* client);
GALAY_C_API galay_status_t galay_redis_client_command(galay_redis_client_t* client,
                                                      const char* command,
                                                      const char* const* args,
                                                      const size_t* arg_lens,
                                                      size_t arg_count,
                                                      galay_redis_reply_t** out_reply);

GALAY_C_END_DECLS

#endif /* GALAY_C_REDIS_REDIS_H */
