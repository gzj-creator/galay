#include <galay/c/galay-mcp-c/mcp_c.h>

#include <stdio.h>

static galay_status_t example_tool(const char* arguments,
                                   size_t arguments_len,
                                   galay_mcp_message_t* result,
                                   void* userdata)
{
    int* calls = (int*)userdata;
    if (arguments == NULL || arguments_len == 0 || result == NULL || calls == NULL) {
        return GALAY_INVALID_ARGUMENT;
    }
    *calls += 1;
    return galay_mcp_message_set_json(
        result,
        "{\"content\":[{\"type\":\"text\",\"text\":\"stdio client example\"}],\"isError\":false}");
}

static int print_message(galay_mcp_message_t* message)
{
    const char* data = NULL;
    size_t data_len = 0;
    if (galay_mcp_message_data(message, &data, &data_len) != GALAY_OK || data == NULL) {
        return 1;
    }
    if (printf("%.*s\n", (int)data_len, data) < 0) {
        return 1;
    }
    return 0;
}

int main(void)
{
    galay_mcp_server_t* server = NULL;
    galay_mcp_client_config_t* config = NULL;
    galay_mcp_client_t* client = NULL;
    galay_mcp_message_t* result = NULL;
    int calls = 0;
    int exit_code = 0;

    if (galay_mcp_stdio_server_create(&server) != GALAY_OK ||
        galay_mcp_server_set_info(server, "example-stdio", "1.0.0") != GALAY_OK ||
        galay_mcp_server_add_tool(server,
                                  "example",
                                  "stdio example tool",
                                  "{\"type\":\"object\"}",
                                  example_tool,
                                  &calls) != GALAY_OK ||
        galay_mcp_stdio_config_create(NULL, NULL, &config) != GALAY_OK ||
        galay_mcp_client_create(config, &client) != GALAY_OK ||
        galay_mcp_client_connect_stdio_loopback(client, server, 1000).code != C_IOResultOk ||
        galay_mcp_client_initialize_async(client, "example-client", "1.0.0", 1000).code != C_IOResultOk ||
        galay_mcp_client_call_tool_async(client, "example", "{}", 1000, &result).code != C_IOResultOk ||
        print_message(result) != 0 ||
        calls != 1) {
        exit_code = 1;
    }

    galay_mcp_message_destroy(result);
    if (client != NULL && galay_mcp_client_disconnect_async(client, 1000).code != C_IOResultOk && exit_code == 0) {
        exit_code = 1;
    }
    galay_mcp_client_destroy(client);
    galay_mcp_client_config_destroy(config);
    galay_mcp_server_destroy(server);
    return exit_code;
}
