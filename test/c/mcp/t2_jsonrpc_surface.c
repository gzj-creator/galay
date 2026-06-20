#include <galay/c/galay-mcp/mcp.h>

#include <stdio.h>
#include <string.h>

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "require failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1; \
        } \
    } while (0)

#define REQUIRE_STATUS(expr, expected) \
    do { \
        galay_status_t got_status = (expr); \
        if (got_status != (expected)) { \
            fprintf(stderr, "status failed: %s:%d: got %d expected %d\n", \
                    __FILE__, __LINE__, (int)got_status, (int)(expected)); \
            return 1; \
        } \
    } while (0)

static int test_request_builder_and_parser(void)
{
    galay_mcp_message_t* message = NULL;
    galay_mcp_parsed_request_t* parsed = NULL;
    const char* data = NULL;
    size_t data_len = 0;
    const char* method = NULL;
    size_t method_len = 0;
    const char* params = NULL;
    size_t params_len = 0;
    int64_t id = 0;

    REQUIRE_STATUS(galay_mcp_message_create(&message), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_build_request(message, 7, "tools/list", "{\"cursor\":\"abc\"}"), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_message_data(message, &data, &data_len), GALAY_OK);
    REQUIRE_TRUE(data != NULL);
    REQUIRE_TRUE(data_len > 0);
    REQUIRE_TRUE(strstr(data, "\"jsonrpc\":\"2.0\"") != NULL);
    REQUIRE_TRUE(strstr(data, "\"id\":7") != NULL);

    REQUIRE_STATUS(galay_mcp_parse_request(data, data_len, &parsed), GALAY_OK);
    REQUIRE_TRUE(galay_mcp_request_is_notification(parsed) == GALAY_FALSE);
    REQUIRE_STATUS(galay_mcp_request_id(parsed, &id), GALAY_OK);
    REQUIRE_TRUE(id == 7);
    REQUIRE_STATUS(galay_mcp_request_method(parsed, &method, &method_len), GALAY_OK);
    REQUIRE_TRUE(method_len == strlen("tools/list"));
    REQUIRE_TRUE(strncmp(method, "tools/list", method_len) == 0);
    REQUIRE_STATUS(galay_mcp_request_params(parsed, &params, &params_len), GALAY_OK);
    REQUIRE_TRUE(params != NULL);
    REQUIRE_TRUE(params_len == strlen("{\"cursor\":\"abc\"}"));
    REQUIRE_TRUE(strncmp(params, "{\"cursor\":\"abc\"}", params_len) == 0);

    galay_mcp_parsed_request_destroy(parsed);
    galay_mcp_message_destroy(message);
    return 0;
}

static int test_notification_without_id_is_request_not_response(void)
{
    galay_mcp_message_t* message = NULL;
    galay_mcp_parsed_request_t* parsed = NULL;
    galay_mcp_parsed_response_t* response = NULL;
    const char* data = NULL;
    size_t data_len = 0;
    int64_t id = 0;

    REQUIRE_STATUS(galay_mcp_message_create(&message), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_build_initialized_notification(message), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_message_data(message, &data, &data_len), GALAY_OK);

    REQUIRE_STATUS(galay_mcp_parse_request(data, data_len, &parsed), GALAY_OK);
    REQUIRE_TRUE(galay_mcp_request_is_notification(parsed) == GALAY_TRUE);
    REQUIRE_STATUS(galay_mcp_request_id(parsed, &id), GALAY_PROTOCOL_ERROR);
    REQUIRE_STATUS(galay_mcp_parse_response(data, data_len, &response), GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(response == NULL);

    galay_mcp_parsed_request_destroy(parsed);
    galay_mcp_message_destroy(message);
    return 0;
}

static int test_response_builder_and_parser(void)
{
    galay_mcp_message_t* message = NULL;
    galay_mcp_parsed_response_t* parsed = NULL;
    const char* data = NULL;
    size_t data_len = 0;
    const char* result = NULL;
    size_t result_len = 0;
    int64_t id = 0;

    REQUIRE_STATUS(galay_mcp_message_create(&message), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_build_empty_result_response(message, 9), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_message_data(message, &data, &data_len), GALAY_OK);
    REQUIRE_TRUE(strstr(data, "\"result\":{}") != NULL);

    REQUIRE_STATUS(galay_mcp_parse_response(data, data_len, &parsed), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_response_id(parsed, &id), GALAY_OK);
    REQUIRE_TRUE(id == 9);
    REQUIRE_TRUE(galay_mcp_response_has_result(parsed) == GALAY_TRUE);
    REQUIRE_STATUS(galay_mcp_response_result(parsed, &result, &result_len), GALAY_OK);
    REQUIRE_TRUE(result_len == 2);
    REQUIRE_TRUE(strncmp(result, "{}", result_len) == 0);

    galay_mcp_parsed_response_destroy(parsed);
    galay_mcp_message_destroy(message);
    return 0;
}

static int test_boundaries_are_rejected(void)
{
    galay_mcp_message_t* message = NULL;
    galay_mcp_parsed_request_t* request = NULL;
    galay_mcp_parsed_response_t* response = NULL;
    const char bad_json[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":";
    const char missing_jsonrpc[] = "{\"id\":1,\"method\":\"ping\"}";
    const char invalid_jsonrpc[] = "{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":{}}";
    const char missing_id_response[] = "{\"jsonrpc\":\"2.0\",\"result\":{}}";
    const char no_result_response[] = "{\"jsonrpc\":\"2.0\",\"id\":3}";

    REQUIRE_STATUS(galay_mcp_message_create(&message), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_build_request(message, 1, "", NULL), GALAY_INVALID_ARGUMENT);
    REQUIRE_STATUS(galay_mcp_build_notification(message, "bad method", NULL), GALAY_INVALID_ARGUMENT);

    REQUIRE_STATUS(galay_mcp_parse_request(bad_json, sizeof(bad_json) - 1, &request),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(request == NULL);
    REQUIRE_STATUS(galay_mcp_parse_request(missing_jsonrpc, sizeof(missing_jsonrpc) - 1, &request),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(request == NULL);
    REQUIRE_STATUS(galay_mcp_parse_response(invalid_jsonrpc, sizeof(invalid_jsonrpc) - 1, &response),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(response == NULL);
    REQUIRE_STATUS(galay_mcp_parse_response(missing_id_response, sizeof(missing_id_response) - 1, &response),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(response == NULL);
    REQUIRE_STATUS(galay_mcp_parse_response(no_result_response, sizeof(no_result_response) - 1, &response),
                   GALAY_PROTOCOL_ERROR);
    REQUIRE_TRUE(response == NULL);

    galay_mcp_message_destroy(message);
    return 0;
}

static int test_mode_configs(void)
{
    galay_mcp_client_config_t* stdio_config = NULL;
    galay_mcp_client_config_t* http_config = NULL;
    const char* url = NULL;
    size_t url_len = 0;

    REQUIRE_STATUS(galay_mcp_stdio_config_create(NULL, NULL, &stdio_config), GALAY_OK);
    REQUIRE_TRUE(galay_mcp_client_config_mode(stdio_config) == GALAY_MCP_MODE_STDIO);

    REQUIRE_STATUS(galay_mcp_http_config_create("https://example.test/mcp", &http_config), GALAY_OK);
    REQUIRE_TRUE(galay_mcp_client_config_mode(http_config) == GALAY_MCP_MODE_HTTP);
    REQUIRE_STATUS(galay_mcp_http_config_url(http_config, &url, &url_len), GALAY_OK);
    REQUIRE_TRUE(url_len == strlen("https://example.test/mcp"));
    REQUIRE_TRUE(strncmp(url, "https://example.test/mcp", url_len) == 0);
    REQUIRE_STATUS(galay_mcp_http_config_create("", &http_config), GALAY_INVALID_ARGUMENT);

    galay_mcp_client_config_destroy(http_config);
    galay_mcp_client_config_destroy(stdio_config);
    return 0;
}

int main(void)
{
    if (test_request_builder_and_parser() != 0) {
        return 1;
    }
    if (test_notification_without_id_is_request_not_response() != 0) {
        return 1;
    }
    if (test_response_builder_and_parser() != 0) {
        return 1;
    }
    if (test_boundaries_are_rejected() != 0) {
        return 1;
    }
    if (test_mode_configs() != 0) {
        return 1;
    }
    return 0;
}
