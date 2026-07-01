#include <galay/c/galay-mcp-c/mcp_c.h>

#include <stdio.h>

static galay_status_t tool_handler(const char* arguments,
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
        "{\"content\":[{\"type\":\"text\",\"text\":\"server tool\"}],\"isError\":false}");
}

static galay_status_t resource_reader(const char* uri,
                                      size_t uri_len,
                                      galay_mcp_message_t* result,
                                      void* userdata)
{
    int* calls = (int*)userdata;
    if (uri == NULL || uri_len == 0 || result == NULL || calls == NULL) {
        return GALAY_INVALID_ARGUMENT;
    }
    *calls += 1;
    return galay_mcp_message_set_json(
        result,
        "{\"contents\":[{\"type\":\"text\",\"text\":\"server resource\"}]}");
}

static galay_status_t prompt_getter(const char* name,
                                    size_t name_len,
                                    const char* arguments,
                                    size_t arguments_len,
                                    galay_mcp_message_t* result,
                                    void* userdata)
{
    int* calls = (int*)userdata;
    if (name == NULL || name_len == 0 || arguments == NULL || arguments_len == 0 ||
        result == NULL || calls == NULL) {
        return GALAY_INVALID_ARGUMENT;
    }
    *calls += 1;
    return galay_mcp_message_set_json(
        result,
        "{\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"server prompt\"}}]}");
}

int main(void)
{
    galay_mcp_server_t* server = NULL;
    int calls = 0;
    int printed = 0;

    if (galay_mcp_stdio_server_create(&server) != GALAY_OK ||
        galay_mcp_server_set_info(server, "example-server", "1.0.0") != GALAY_OK ||
        galay_mcp_server_add_tool(server,
                                  "tool",
                                  "registered tool",
                                  "{\"type\":\"object\"}",
                                  tool_handler,
                                  &calls) != GALAY_OK ||
        galay_mcp_server_add_resource(server,
                                      "file:///example.txt",
                                      "example resource",
                                      "registered resource",
                                      "text/plain",
                                      resource_reader,
                                      &calls) != GALAY_OK ||
        galay_mcp_server_add_prompt(server,
                                    "prompt",
                                    "registered prompt",
                                    "[{\"name\":\"topic\",\"required\":false}]",
                                    prompt_getter,
                                    &calls) != GALAY_OK) {
        galay_mcp_server_destroy(server);
        return 1;
    }

    printed = printf("stdio MCP server registered tool/resource/prompt handlers\n");
    galay_mcp_server_destroy(server);
    return printed < 0 ? 1 : 0;
}
