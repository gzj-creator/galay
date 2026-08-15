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

typedef struct HandlerCounts {
    int tool_calls;
    int resource_reads;
    int prompt_gets;
} HandlerCounts;

static galay_status_t tool_handler(const char* arguments,
                                   size_t arguments_len,
                                   galay_mcp_message_t* result,
                                   void* userdata)
{
    HandlerCounts* counts = (HandlerCounts*)userdata;
    if (counts == NULL || arguments == NULL || arguments_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    counts->tool_calls += 1;
    return galay_mcp_message_set_json(
        result,
        "{\"content\":[{\"type\":\"text\",\"text\":\"tool-result\"}],\"isError\":false}");
}

static galay_status_t resource_reader(const char* uri,
                                      size_t uri_len,
                                      galay_mcp_message_t* result,
                                      void* userdata)
{
    HandlerCounts* counts = (HandlerCounts*)userdata;
    if (counts == NULL || uri == NULL || uri_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    counts->resource_reads += 1;
    return galay_mcp_message_set_json(
        result,
        "{\"contents\":[{\"type\":\"text\",\"text\":\"resource-result\"}]}");
}

static galay_status_t prompt_getter(const char* name,
                                    size_t name_len,
                                    const char* arguments,
                                    size_t arguments_len,
                                    galay_mcp_message_t* result,
                                    void* userdata)
{
    HandlerCounts* counts = (HandlerCounts*)userdata;
    if (counts == NULL || name == NULL || name_len == 0 || arguments == NULL || arguments_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    counts->prompt_gets += 1;
    return galay_mcp_message_set_json(
        result,
        "{\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"prompt-result\"}}]}");
}

static int message_contains(galay_mcp_message_t* message, const char* needle)
{
    const char* data = NULL;
    size_t data_len = 0;
    if (galay_mcp_message_data(message, &data, &data_len) != GALAY_OK || data == NULL) {
        return 0;
    }
    return strstr(data, needle) != NULL;
}

int main(void)
{
    galay_mcp_server_t* server = NULL;
    galay_mcp_client_config_t* config = NULL;
    galay_mcp_client_t* client = NULL;
    galay_mcp_message_t* tool_result = NULL;
    galay_mcp_message_t* resource_list = NULL;
    galay_mcp_message_t* resource_result = NULL;
    galay_mcp_message_t* prompt_list = NULL;
    galay_mcp_message_t* prompt_result = NULL;
    HandlerCounts counts = {0};

    REQUIRE_STATUS(galay_mcp_stdio_server_create(&server), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_server_set_info(server, "handler-server", "1.0.0"), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_server_add_tool(server,
                                             "tool_a",
                                             "tool handler",
                                             "{\"type\":\"object\"}",
                                             tool_handler,
                                             &counts),
                   GALAY_OK);
    REQUIRE_STATUS(galay_mcp_server_add_resource(server,
                                                 "file:///handler.txt",
                                                 "handler resource",
                                                 "resource handler",
                                                 "text/plain",
                                                 resource_reader,
                                                 &counts),
                   GALAY_OK);
    REQUIRE_STATUS(galay_mcp_server_add_prompt(server,
                                               "prompt_a",
                                               "prompt handler",
                                               "[{\"name\":\"topic\",\"required\":true}]",
                                               prompt_getter,
                                               &counts),
                   GALAY_OK);

    REQUIRE_STATUS(galay_mcp_stdio_config_create(NULL, NULL, &config), GALAY_OK);
    REQUIRE_STATUS(galay_mcp_client_create(config, &client), GALAY_OK);
    REQUIRE_IO(galay_mcp_client_connect_stdio_loopback(client, server, 1000));
    REQUIRE_IO(galay_mcp_client_initialize_async(client, "handler-test", "1.0.0", 1000));

    REQUIRE_IO(galay_mcp_client_call_tool_async(client, "tool_a", "{\"x\":1}", 1000, &tool_result));
    REQUIRE_TRUE(message_contains(tool_result, "tool-result"));
    REQUIRE_IO(galay_mcp_client_list_resources_async(client, 1000, &resource_list));
    REQUIRE_TRUE(message_contains(resource_list, "file:///handler.txt"));
    REQUIRE_IO(galay_mcp_client_read_resource_async(client, "file:///handler.txt", 1000, &resource_result));
    REQUIRE_TRUE(message_contains(resource_result, "resource-result"));
    REQUIRE_IO(galay_mcp_client_list_prompts_async(client, 1000, &prompt_list));
    REQUIRE_TRUE(message_contains(prompt_list, "prompt_a"));
    REQUIRE_IO(galay_mcp_client_get_prompt_async(client, "prompt_a", "{\"topic\":\"c\"}", 1000, &prompt_result));
    REQUIRE_TRUE(message_contains(prompt_result, "prompt-result"));
    REQUIRE_TRUE(counts.tool_calls == 1);
    REQUIRE_TRUE(counts.resource_reads == 1);
    REQUIRE_TRUE(counts.prompt_gets == 1);

    galay_mcp_message_destroy(prompt_result);
    galay_mcp_message_destroy(prompt_list);
    galay_mcp_message_destroy(resource_result);
    galay_mcp_message_destroy(resource_list);
    galay_mcp_message_destroy(tool_result);
    galay_mcp_client_destroy(client);
    galay_mcp_client_config_destroy(config);
    galay_mcp_server_destroy(server);
    return 0;
}
