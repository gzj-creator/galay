#include <galay/c/galay-mcp-c/mcp.h>

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

#define REQUIRE_IO(expr) \
    do { \
        C_IOResult got_result = (expr); \
        if (got_result.code != C_IOResultOk) { \
            fprintf(stderr, "io failed: %s:%d: got %d value %lld\n", \
                    __FILE__, __LINE__, (int)got_result.code, (long long)got_result.value); \
            return 1; \
        } \
    } while (0)

static galay_status_t echo_tool(const char* arguments,
                                size_t arguments_len,
                                galay_mcp_message_t* result,
                                void* userdata)
{
    int* call_count = (int*)userdata;
    if (call_count == NULL || arguments == NULL || arguments_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    *call_count += 1;
    return galay_mcp_message_set_json(
        result,
        "{\"content\":[{\"type\":\"text\",\"text\":\"stdio-echo\"}],\"isError\":false}");
}

int main(void)
{
    galay_mcp_server_t* server = NULL;
    galay_mcp_client_config_t* config = NULL;
    galay_mcp_client_t* client = NULL;
    galay_mcp_message_t* result = NULL;
    const char* data = NULL;
    size_t data_len = 0;
    int call_count = 0;

    REQUIRE_STATUS(galay_mcp_stdio_server_create(&server), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_server_set_info(server, "c-stdio-loopback", "1.0.0"), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_server_add_tool(server,
                                             "echo",
                                             "echo test tool",
                                             "{\"type\":\"object\"}",
                                             echo_tool,
                                             &call_count),
                   GALAY_OK);

    REQUIRE_STATUS(galay_mcp_stdio_config_create(NULL, NULL, &config), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_client_create(config, &client), GALAY_OK);
    REQUIRE_IO(galay_mcp_client_connect_stdio_loopback(client, server, 1000));
    REQUIRE_IO(galay_mcp_client_initialize_async(client, "c-test", "1.0.0", 1000));
    REQUIRE_IO(galay_mcp_client_ping_async(client, 1000));

    REQUIRE_IO(galay_mcp_client_list_tools_async(client, 1000, &result));
    REQUIRE_STATUS(galay_mcp_message_data(result, &data, &data_len), GALAY_OK);
    REQUIRE_TRUE(data != NULL);
    REQUIRE_TRUE(data_len > 0);
    REQUIRE_TRUE(strstr(data, "\"tools\"") != NULL);
    REQUIRE_TRUE(strstr(data, "\"echo\"") != NULL);
    galay_mcp_message_destroy(result);
    result = NULL;

    REQUIRE_IO(galay_mcp_client_call_tool_async(client, "echo", "{\"value\":7}", 1000, &result));
    REQUIRE_STATUS(galay_mcp_message_data(result, &data, &data_len), GALAY_OK);
    REQUIRE_TRUE(data != NULL);
    REQUIRE_TRUE(strstr(data, "stdio-echo") != NULL);
    REQUIRE_TRUE(call_count == 1);

    REQUIRE_IO(galay_mcp_client_disconnect_async(client, 1000));
    REQUIRE_TRUE(galay_mcp_client_is_connected(client) == GALAY_FALSE);

    galay_mcp_message_destroy(result);
    galay_mcp_client_destroy(client);
    galay_mcp_client_config_destroy(config);
    galay_mcp_server_destroy(server);
    return 0;
}
