/**
 * @file mcp.h
 * @brief galay-mcp C ABI 封装。
 *
 * @details C API 只暴露 opaque handle、稳定枚举和显式错误码。返回的
 *          const char* 指针由对应 handle 持有，在下一次修改该 handle
 *          或 destroy 前有效，调用方不得释放。
 */

#ifndef GALAY_C_MCP_MCP_H
#define GALAY_C_MCP_MCP_H

#include <galay/c/galay-c/common/galay_c_error.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

GALAY_C_BEGIN_DECLS

typedef struct galay_mcp_message galay_mcp_message_t;
typedef struct galay_mcp_parsed_request galay_mcp_parsed_request_t;
typedef struct galay_mcp_parsed_response galay_mcp_parsed_response_t;
typedef struct galay_mcp_client_config galay_mcp_client_config_t;

typedef enum galay_mcp_client_mode {
    GALAY_MCP_MODE_STDIO = 0,
    GALAY_MCP_MODE_HTTP = 1,
    GALAY_MCP_MODE_INVALID = 2
} galay_mcp_client_mode_t;

GALAY_C_API galay_status_t galay_mcp_message_create(galay_mcp_message_t** out);
GALAY_C_API void galay_mcp_message_destroy(galay_mcp_message_t* message);
GALAY_C_API void galay_mcp_message_reset(galay_mcp_message_t* message);
GALAY_C_API galay_status_t galay_mcp_message_data(const galay_mcp_message_t* message,
                                                  const char** data,
                                                  size_t* data_len);

GALAY_C_API galay_status_t galay_mcp_build_request(galay_mcp_message_t* message,
                                                   int64_t id,
                                                   const char* method,
                                                   const char* params_json);
GALAY_C_API galay_status_t galay_mcp_build_notification(galay_mcp_message_t* message,
                                                        const char* method,
                                                        const char* params_json);
GALAY_C_API galay_status_t galay_mcp_build_response(galay_mcp_message_t* message,
                                                    int64_t id,
                                                    const char* result_json);
GALAY_C_API galay_status_t galay_mcp_build_error_response(galay_mcp_message_t* message,
                                                          int64_t id,
                                                          int error_code,
                                                          const char* error_message,
                                                          const char* data_json);

GALAY_C_API galay_status_t galay_mcp_build_initialize_request(galay_mcp_message_t* message,
                                                              int64_t id,
                                                              const char* client_name,
                                                              const char* client_version,
                                                              const char* capabilities_json);
GALAY_C_API galay_status_t galay_mcp_build_ping_request(galay_mcp_message_t* message,
                                                        int64_t id);
GALAY_C_API galay_status_t galay_mcp_build_initialized_notification(galay_mcp_message_t* message);
GALAY_C_API galay_status_t galay_mcp_build_empty_result_response(galay_mcp_message_t* message,
                                                                 int64_t id);

GALAY_C_API galay_status_t galay_mcp_parse_request(const void* data,
                                                   size_t data_len,
                                                   galay_mcp_parsed_request_t** out);
GALAY_C_API void galay_mcp_parsed_request_destroy(galay_mcp_parsed_request_t* request);
GALAY_C_API galay_bool_t galay_mcp_request_is_notification(const galay_mcp_parsed_request_t* request);
GALAY_C_API galay_status_t galay_mcp_request_id(const galay_mcp_parsed_request_t* request,
                                                int64_t* id);
GALAY_C_API galay_status_t galay_mcp_request_method(const galay_mcp_parsed_request_t* request,
                                                    const char** method,
                                                    size_t* method_len);
GALAY_C_API galay_status_t galay_mcp_request_params(const galay_mcp_parsed_request_t* request,
                                                    const char** params,
                                                    size_t* params_len);

GALAY_C_API galay_status_t galay_mcp_parse_response(const void* data,
                                                    size_t data_len,
                                                    galay_mcp_parsed_response_t** out);
GALAY_C_API void galay_mcp_parsed_response_destroy(galay_mcp_parsed_response_t* response);
GALAY_C_API galay_status_t galay_mcp_response_id(const galay_mcp_parsed_response_t* response,
                                                 int64_t* id);
GALAY_C_API galay_bool_t galay_mcp_response_has_result(const galay_mcp_parsed_response_t* response);
GALAY_C_API galay_status_t galay_mcp_response_result(const galay_mcp_parsed_response_t* response,
                                                     const char** result,
                                                     size_t* result_len);
GALAY_C_API galay_bool_t galay_mcp_response_has_error(const galay_mcp_parsed_response_t* response);
GALAY_C_API galay_status_t galay_mcp_response_error(const galay_mcp_parsed_response_t* response,
                                                    const char** error,
                                                    size_t* error_len);

GALAY_C_API galay_status_t galay_mcp_stdio_config_create(FILE* input,
                                                         FILE* output,
                                                         galay_mcp_client_config_t** out);
GALAY_C_API galay_status_t galay_mcp_http_config_create(const char* url,
                                                        galay_mcp_client_config_t** out);
GALAY_C_API void galay_mcp_client_config_destroy(galay_mcp_client_config_t* config);
GALAY_C_API galay_mcp_client_mode_t galay_mcp_client_config_mode(const galay_mcp_client_config_t* config);
GALAY_C_API FILE* galay_mcp_stdio_config_input(const galay_mcp_client_config_t* config);
GALAY_C_API FILE* galay_mcp_stdio_config_output(const galay_mcp_client_config_t* config);
GALAY_C_API galay_status_t galay_mcp_http_config_url(const galay_mcp_client_config_t* config,
                                                     const char** url,
                                                     size_t* url_len);

GALAY_C_END_DECLS

#endif /* GALAY_C_MCP_MCP_H */
