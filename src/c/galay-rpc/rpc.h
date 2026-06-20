/**
 * @file rpc.h
 * @brief Galay RPC C ABI 请求/响应 envelope 编解码接口。
 *
 * @details 该头文件只暴露 C 兼容的值类型、稳定枚举和显式错误码。
 *          decode 输出的 service、method 和 payload 指针借用输入 buffer，
 *          在输入 buffer 保持有效且未被修改前有效，调用方不得释放。
 */

#ifndef GALAY_C_RPC_RPC_H
#define GALAY_C_RPC_RPC_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

enum {
    GALAY_RPC_HEADER_SIZE = 16,
    GALAY_RPC_MAX_BODY_SIZE = 16 * 1024 * 1024
};

/**
 * @brief RPC 调用模式。
 */
typedef enum galay_rpc_call_mode {
    GALAY_RPC_CALL_UNARY = 0,
    GALAY_RPC_CALL_CLIENT_STREAMING = 1,
    GALAY_RPC_CALL_SERVER_STREAMING = 2,
    GALAY_RPC_CALL_BIDI_STREAMING = 3
} galay_rpc_call_mode_t;

/**
 * @brief RPC 协议错误码，数值与 C++ RpcErrorCode 保持一致。
 */
typedef enum galay_rpc_error_code {
    GALAY_RPC_ERROR_OK = 0,
    GALAY_RPC_ERROR_UNKNOWN_ERROR = 1,
    GALAY_RPC_ERROR_SERVICE_NOT_FOUND = 2,
    GALAY_RPC_ERROR_METHOD_NOT_FOUND = 3,
    GALAY_RPC_ERROR_INVALID_REQUEST = 4,
    GALAY_RPC_ERROR_INVALID_RESPONSE = 5,
    GALAY_RPC_ERROR_REQUEST_TIMEOUT = 6,
    GALAY_RPC_ERROR_CONNECTION_CLOSED = 7,
    GALAY_RPC_ERROR_SERIALIZATION_ERROR = 8,
    GALAY_RPC_ERROR_DESERIALIZATION_ERROR = 9,
    GALAY_RPC_ERROR_INTERNAL_ERROR = 10
} galay_rpc_error_code_t;

/**
 * @brief RPC 请求 envelope。
 * @note encode 时字段为输入；decode 时 service、method、payload 借用输入 buffer。
 */
typedef struct galay_rpc_request {
    uint32_t request_id;
    galay_rpc_call_mode_t call_mode;
    galay_bool_t end_of_stream;
    const char* service;
    size_t service_len;
    const char* method;
    size_t method_len;
    const void* payload;
    size_t payload_len;
} galay_rpc_request_t;

/**
 * @brief RPC 响应 envelope。
 * @note encode 时字段为输入；decode 时 payload 借用输入 buffer。
 */
typedef struct galay_rpc_response {
    uint32_t request_id;
    galay_rpc_call_mode_t call_mode;
    galay_bool_t end_of_stream;
    galay_rpc_error_code_t error_code;
    const void* payload;
    size_t payload_len;
} galay_rpc_response_t;

/**
 * @brief 返回 RPC 错误码的静态字符串。
 */
GALAY_C_API const char* galay_rpc_error_string(galay_rpc_error_code_t error);

/**
 * @brief 将 RPC 错误码映射为公共 C ABI 状态码。
 */
GALAY_C_API galay_status_t galay_rpc_error_to_status(galay_rpc_error_code_t error);

/**
 * @brief 验证服务名或方法名。
 * @details 名称必须非空，以字母或下划线开头，后续字符可为字母、数字、下划线、点或横线。
 */
GALAY_C_API galay_bool_t galay_rpc_name_is_valid(const char* name, size_t name_len);

/**
 * @brief 计算请求 envelope 编码后所需字节数。
 */
GALAY_C_API galay_status_t galay_rpc_request_encoded_size(const galay_rpc_request_t* request,
                                                          size_t* out_size);

/**
 * @brief 编码请求 envelope。
 */
GALAY_C_API galay_status_t galay_rpc_encode_request(const galay_rpc_request_t* request,
                                                    void* out,
                                                    size_t out_capacity,
                                                    size_t* written);

/**
 * @brief 解码请求 envelope。
 * @param consumed 成功时输出完整消息长度，失败时置 0；可为 NULL。
 * @param rpc_error 失败时输出 RPC 协议错误码；可为 NULL。
 */
GALAY_C_API galay_status_t galay_rpc_decode_request(const void* data,
                                                    size_t data_len,
                                                    galay_rpc_request_t* out_request,
                                                    size_t* consumed,
                                                    galay_rpc_error_code_t* rpc_error);

/**
 * @brief 计算响应 envelope 编码后所需字节数。
 */
GALAY_C_API galay_status_t galay_rpc_response_encoded_size(const galay_rpc_response_t* response,
                                                           size_t* out_size);

/**
 * @brief 编码响应 envelope。
 */
GALAY_C_API galay_status_t galay_rpc_encode_response(const galay_rpc_response_t* response,
                                                     void* out,
                                                     size_t out_capacity,
                                                     size_t* written);

/**
 * @brief 解码响应 envelope。
 * @param consumed 成功时输出完整消息长度，失败时置 0；可为 NULL。
 * @param rpc_error 失败时输出 RPC 协议错误码；可为 NULL。
 */
GALAY_C_API galay_status_t galay_rpc_decode_response(const void* data,
                                                     size_t data_len,
                                                     galay_rpc_response_t* out_response,
                                                     size_t* consumed,
                                                     galay_rpc_error_code_t* rpc_error);

GALAY_C_END_DECLS

#endif /* GALAY_C_RPC_RPC_H */
