#ifndef GALAY_C_MCP_MCP_H
#define GALAY_C_MCP_MCP_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

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
typedef struct galay_mcp_client_t galay_mcp_client_t;
typedef struct galay_mcp_server_t galay_mcp_server_t;

typedef galay_status_t (*galay_mcp_tool_handler_fn)(const char* arguments,
                                                    size_t arguments_len,
                                                    galay_mcp_message_t* result,
                                                    void* userdata);
typedef galay_status_t (*galay_mcp_resource_reader_fn)(const char* uri,
                                                       size_t uri_len,
                                                       galay_mcp_message_t* result,
                                                       void* userdata);
typedef galay_status_t (*galay_mcp_prompt_getter_fn)(const char* name,
                                                     size_t name_len,
                                                     const char* arguments,
                                                     size_t arguments_len,
                                                     galay_mcp_message_t* result,
                                                     void* userdata);

const char* galay_mcp_get_error(galay_status_t status);
galay_status_t galay_mcp_message_create(galay_mcp_message_t** out);
void galay_mcp_message_destroy(galay_mcp_message_t* message);
galay_status_t galay_mcp_message_set_json(galay_mcp_message_t* message, const char* json);
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
galay_status_t galay_mcp_http_config_set_bearer_token(galay_mcp_client_config_t* config,
                                                      const char* bearer_token);
void galay_mcp_client_config_destroy(galay_mcp_client_config_t* config);
galay_mcp_mode_t galay_mcp_client_config_mode(const galay_mcp_client_config_t* config);
galay_status_t galay_mcp_http_config_url(const galay_mcp_client_config_t* config, const char** url, size_t* url_len);
galay_status_t galay_mcp_client_create(const galay_mcp_client_config_t* config, galay_mcp_client_t** out);
void galay_mcp_client_destroy(galay_mcp_client_t* client);
galay_bool_t galay_mcp_client_is_connected(const galay_mcp_client_t* client);
C_IOResult galay_mcp_client_connect_stdio_loopback(galay_mcp_client_t* client,
                                                   galay_mcp_server_t* server,
                                                   int64_t timeout_ms);
C_IOResult galay_mcp_client_connect_async(galay_mcp_client_t* client, int64_t timeout_ms);
C_IOResult galay_mcp_client_disconnect_async(galay_mcp_client_t* client, int64_t timeout_ms);
C_IOResult galay_mcp_client_initialize_async(galay_mcp_client_t* client,
                                             const char* client_name,
                                             const char* client_version,
                                             int64_t timeout_ms);
C_IOResult galay_mcp_client_ping_async(galay_mcp_client_t* client, int64_t timeout_ms);
C_IOResult galay_mcp_client_list_tools_async(galay_mcp_client_t* client,
                                             int64_t timeout_ms,
                                             galay_mcp_message_t** result);
C_IOResult galay_mcp_client_call_tool_async(galay_mcp_client_t* client,
                                            const char* tool_name,
                                            const char* arguments_json,
                                            int64_t timeout_ms,
                                            galay_mcp_message_t** result);
C_IOResult galay_mcp_client_list_resources_async(galay_mcp_client_t* client,
                                                 int64_t timeout_ms,
                                                 galay_mcp_message_t** result);
C_IOResult galay_mcp_client_read_resource_async(galay_mcp_client_t* client,
                                                const char* uri,
                                                int64_t timeout_ms,
                                                galay_mcp_message_t** result);
C_IOResult galay_mcp_client_list_prompts_async(galay_mcp_client_t* client,
                                               int64_t timeout_ms,
                                               galay_mcp_message_t** result);
C_IOResult galay_mcp_client_get_prompt_async(galay_mcp_client_t* client,
                                             const char* name,
                                             const char* arguments_json,
                                             int64_t timeout_ms,
                                             galay_mcp_message_t** result);
galay_status_t galay_mcp_stdio_server_create(galay_mcp_server_t** out);
galay_status_t galay_mcp_http_server_create(const char* host, uint16_t port, galay_mcp_server_t** out);
void galay_mcp_server_destroy(galay_mcp_server_t* server);
galay_status_t galay_mcp_server_set_info(galay_mcp_server_t* server,
                                         const char* name,
                                         const char* version);
galay_status_t galay_mcp_http_server_require_bearer_token(galay_mcp_server_t* server,
                                                          const char* bearer_token);
galay_status_t galay_mcp_server_add_tool(galay_mcp_server_t* server,
                                         const char* name,
                                         const char* description,
                                         const char* input_schema_json,
                                         galay_mcp_tool_handler_fn handler,
                                         void* userdata);
galay_status_t galay_mcp_server_add_resource(galay_mcp_server_t* server,
                                             const char* uri,
                                             const char* name,
                                             const char* description,
                                             const char* mime_type,
                                             galay_mcp_resource_reader_fn reader,
                                             void* userdata);
galay_status_t galay_mcp_server_add_prompt(galay_mcp_server_t* server,
                                           const char* name,
                                           const char* description,
                                           const char* arguments_json,
                                           galay_mcp_prompt_getter_fn getter,
                                           void* userdata);
galay_status_t galay_mcp_http_server_start(galay_mcp_server_t* server);
galay_status_t galay_mcp_http_server_endpoint(const galay_mcp_server_t* server,
                                              const char** host,
                                              uint16_t* port);
C_IOResult galay_mcp_http_server_serve_once(galay_mcp_server_t* server, int64_t timeout_ms);
C_IOResult galay_mcp_http_server_stop(galay_mcp_server_t* server);

#ifdef __cplusplus
}
#endif

#endif
