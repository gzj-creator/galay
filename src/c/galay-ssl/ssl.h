/**
 * @file ssl.h
 * @brief Galay SSL C ABI 上下文封装。
 */

#ifndef GALAY_C_GALAY_SSL_SSL_H
#define GALAY_C_GALAY_SSL_SSL_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

/**
 * @brief 不透明 SSL 上下文句柄。
 *
 * @details 由 galay_ssl_context_create 创建，必须由
 * galay_ssl_context_destroy 释放。句柄不暴露任何 C++ 类型。
 */
typedef struct galay_ssl_context galay_ssl_context_t;

/**
 * @brief SSL/TLS 协议方法。
 */
typedef enum galay_ssl_method {
    GALAY_SSL_METHOD_TLS_CLIENT = 0,
    GALAY_SSL_METHOD_TLS_SERVER = 1,
    GALAY_SSL_METHOD_TLS_1_2_CLIENT = 2,
    GALAY_SSL_METHOD_TLS_1_2_SERVER = 3,
    GALAY_SSL_METHOD_TLS_1_3_CLIENT = 4,
    GALAY_SSL_METHOD_TLS_1_3_SERVER = 5,
    GALAY_SSL_METHOD_DTLS_CLIENT = 6,
    GALAY_SSL_METHOD_DTLS_SERVER = 7
} galay_ssl_method_t;

/**
 * @brief 证书验证模式。
 */
typedef enum galay_ssl_verify_mode {
    GALAY_SSL_VERIFY_NONE = 0,
    GALAY_SSL_VERIFY_PEER = 1,
    GALAY_SSL_VERIFY_FAIL_IF_NO_PEER_CERT = 2,
    GALAY_SSL_VERIFY_CLIENT_ONCE = 4
} galay_ssl_verify_mode_t;

/**
 * @brief 创建 SSL 上下文。
 * @param method 协议方法。
 * @param out_context 输出新建句柄；失败时写入 NULL。
 * @return GALAY_OK 表示成功；参数无效返回 GALAY_INVALID_ARGUMENT。
 */
GALAY_C_API galay_status_t galay_ssl_context_create(
    galay_ssl_method_t method,
    galay_ssl_context_t** out_context);

/**
 * @brief 销毁 SSL 上下文。
 * @param context 可为 NULL；NULL 时无操作。
 */
GALAY_C_API void galay_ssl_context_destroy(galay_ssl_context_t* context);

/**
 * @brief 加载 PEM 证书文件。
 * @return 缺失文件返回 GALAY_NOT_FOUND；OpenSSL 加载失败返回 GALAY_IO_ERROR。
 */
GALAY_C_API galay_status_t galay_ssl_context_load_certificate(
    galay_ssl_context_t* context,
    const char* cert_file);

/**
 * @brief 加载 PEM 私钥文件。
 * @return 缺失文件返回 GALAY_NOT_FOUND；OpenSSL 加载失败返回 GALAY_IO_ERROR。
 */
GALAY_C_API galay_status_t galay_ssl_context_load_private_key(
    galay_ssl_context_t* context,
    const char* key_file);

/**
 * @brief 加载 CA 证书文件。
 * @return 缺失文件返回 GALAY_NOT_FOUND；OpenSSL 加载失败返回 GALAY_IO_ERROR。
 */
GALAY_C_API galay_status_t galay_ssl_context_load_ca(
    galay_ssl_context_t* context,
    const char* ca_file);

/**
 * @brief 设置证书验证模式。
 * @return 非法 mode 返回 GALAY_INVALID_ARGUMENT。
 */
GALAY_C_API galay_status_t galay_ssl_context_set_verify_mode(
    galay_ssl_context_t* context,
    galay_ssl_verify_mode_t mode);

GALAY_C_END_DECLS

#endif /* GALAY_C_GALAY_SSL_SSL_H */
