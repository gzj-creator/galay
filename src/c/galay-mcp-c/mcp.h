#ifndef GALAY_C_MCP_MCP_H
#define GALAY_C_MCP_MCP_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_mcp_mode_t {
    GALAY_MCP_MODE_STDIO = 0,
    GALAY_MCP_MODE_HTTP = 1
} galay_mcp_mode_t;

typedef struct galay_mcp_message_t galay_mcp_message_t;
typedef struct galay_mcp_parsed_request_t galay_mcp_parsed_request_t;
typedef struct galay_mcp_parsed_response_t galay_mcp_parsed_response_t;
typedef struct galay_mcp_client_config_t galay_mcp_client_config_t;

const char* galay_mcp_get_error(galay_status_t status);
galay_status_t galay_mcp_message_create(galay_mcp_message_t** out);
void galay_mcp_message_destroy(galay_mcp_message_t* message);
galay_status_t galay_mcp_message_data(const galay_mcp_message_t* message, const char** data, size_t* data_len);
galay_status_t galay_mcp_build_request(galay_mcp_message_t* message, int64_t id, const char* method, const char* params);
galay_status_t galay_mcp_build_notification(galay_mcp_message_t* message, const char* method, const char* params);
galay_status_t galay_mcp_build_initialized_notification(galay_mcp_message_t* message);
galay_status_t galay_mcp_build_empty_result_response(galay_mcp_message_t* message, int64_t id);
galay_status_t galay_mcp_parse_request(const char* data, size_t data_len, galay_mcp_parsed_request_t** out);
void galay_mcp_parsed_request_destroy(galay_mcp_parsed_request_t* request);
galay_bool_t galay_mcp_request_is_notification(const galay_mcp_parsed_request_t* request);
galay_status_t galay_mcp_request_id(const galay_mcp_parsed_request_t* request, int64_t* id);
galay_status_t galay_mcp_request_method(const galay_mcp_parsed_request_t* request, const char** method, size_t* method_len);
galay_status_t galay_mcp_request_params(const galay_mcp_parsed_request_t* request, const char** params, size_t* params_len);
galay_status_t galay_mcp_parse_response(const char* data, size_t data_len, galay_mcp_parsed_response_t** out);
void galay_mcp_parsed_response_destroy(galay_mcp_parsed_response_t* response);
galay_status_t galay_mcp_response_id(const galay_mcp_parsed_response_t* response, int64_t* id);
galay_bool_t galay_mcp_response_has_result(const galay_mcp_parsed_response_t* response);
galay_status_t galay_mcp_response_result(const galay_mcp_parsed_response_t* response, const char** result, size_t* result_len);
galay_status_t galay_mcp_stdio_config_create(const char* command, const char* args, galay_mcp_client_config_t** out);
galay_status_t galay_mcp_http_config_create(const char* url, galay_mcp_client_config_t** out);
void galay_mcp_client_config_destroy(galay_mcp_client_config_t* config);
galay_mcp_mode_t galay_mcp_client_config_mode(const galay_mcp_client_config_t* config);
galay_status_t galay_mcp_http_config_url(const galay_mcp_client_config_t* config, const char** url, size_t* url_len);

#ifdef __cplusplus
}
#endif

#endif
