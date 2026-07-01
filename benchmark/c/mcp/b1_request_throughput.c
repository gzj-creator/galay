#include <galay/c/galay-mcp-c/mcp_c.h>

#include <stdio.h>
#include <time.h>

static galay_status_t bench_tool(const char* arguments,
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
        "{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"isError\":false}");
}

int main(void)
{
    const int iterations = 1000;
    galay_mcp_server_t* server = NULL;
    galay_mcp_client_config_t* config = NULL;
    galay_mcp_client_t* client = NULL;
    int calls = 0;
    clock_t started = 0;
    clock_t finished = 0;
    int exit_code = 0;

    if (galay_mcp_stdio_server_create(&server) != GALAY_OK ||
        galay_mcp_server_add_tool(server,
                                  "bench",
                                  "benchmark tool",
                                  "{\"type\":\"object\"}",
                                  bench_tool,
                                  &calls) != GALAY_OK ||
        galay_mcp_stdio_config_create(NULL, NULL, &config) != GALAY_OK ||
        galay_mcp_client_create(config, &client) != GALAY_OK ||
        galay_mcp_client_connect_stdio_loopback(client, server, 1000).code != C_IOResultOk ||
        galay_mcp_client_initialize_async(client, "bench", "1.0.0", 1000).code != C_IOResultOk) {
        exit_code = 1;
        goto cleanup;
    }

    started = clock();
    for (int index = 0; index < iterations; ++index) {
        galay_mcp_message_t* result = NULL;
        C_IOResult call = galay_mcp_client_call_tool_async(client, "bench", "{}", 1000, &result);
        galay_mcp_message_destroy(result);
        if (call.code != C_IOResultOk) {
            exit_code = 1;
            goto cleanup;
        }
    }
    finished = clock();
    if (calls != iterations) {
        exit_code = 1;
        goto cleanup;
    }
    if (printf("mcp stdio call throughput: %.2f ops/sec\n",
               (double)iterations / ((double)(finished - started) / CLOCKS_PER_SEC)) < 0) {
        exit_code = 1;
    }

cleanup:
    galay_mcp_client_destroy(client);
    galay_mcp_client_config_destroy(config);
    galay_mcp_server_destroy(server);
    return exit_code;
}
