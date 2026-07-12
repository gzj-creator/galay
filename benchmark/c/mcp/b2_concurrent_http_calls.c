#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-mcp-c/mcp_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct BenchmarkState {
    galay_mcp_server_t* server;
    galay_mcp_client_t* client;
    int requests;
    int calls;
    int failed;
} BenchmarkState;

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

static void server_entry(void* arg)
{
    BenchmarkState* state = (BenchmarkState*)arg;
    for (int index = 0; index < state->requests; ++index) {
        C_IOResult served = galay_mcp_http_server_serve_once(state->server, 1000);
        if (served.code != C_IOResultOk) {
            state->failed = 1;
            return;
        }
    }
}

static void client_entry(void* arg)
{
    BenchmarkState* state = (BenchmarkState*)arg;
    if (galay_mcp_client_connect_async(state->client, 1000).code != C_IOResultOk ||
        galay_mcp_client_initialize_async(state->client, "http-bench", "1.0.0", 1000).code != C_IOResultOk) {
        state->failed = 1;
        return;
    }
    for (int index = 0; index < state->requests - 1; ++index) {
        galay_mcp_message_t* result = NULL;
        C_IOResult called =
            galay_mcp_client_call_tool_async(state->client, "bench", "{}", 1000, &result);
        galay_mcp_message_destroy(result);
        if (called.code != C_IOResultOk) {
            state->failed = 1;
            return;
        }
    }
}

static double elapsed_seconds(struct timespec started, struct timespec finished)
{
    return (double)(finished.tv_sec - started.tv_sec) +
        (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
}

int main(int argc, char** argv)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    galay_mcp_server_t* server = NULL;
    galay_mcp_client_config_t* config = NULL;
    galay_mcp_client_t* client = NULL;
    BenchmarkState state = {0};
    const char* host = NULL;
    uint16_t port = 0;
    char url[128];
    struct timespec started = {0};
    struct timespec finished = {0};
    int64_t join_timeout_ms = 5000;
    int exit_code = 0;

    state.requests = 1000;
    if (argc > 1) {
        char* end = NULL;
        const long parsed = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || parsed < 2 || parsed > 1000000L) {
            return 2;
        }
        state.requests = (int)parsed;
    }
    join_timeout_ms += (int64_t)state.requests * 2;

    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;
    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_mcp_http_server_create("127.0.0.1", 0, &server) != GALAY_OK ||
        galay_mcp_server_add_tool(server,
                                  "bench",
                                  "HTTP benchmark tool",
                                  "{\"type\":\"object\"}",
                                  bench_tool,
                                  &state.calls) != GALAY_OK ||
        galay_mcp_http_server_start(server) != GALAY_OK ||
        galay_mcp_http_server_endpoint(server, &host, &port) != GALAY_OK ||
        host == NULL ||
        snprintf(url, sizeof(url), "http://%s:%u/mcp", host, (unsigned)port) < 0 ||
        galay_mcp_http_config_create(url, &config) != GALAY_OK ||
        galay_mcp_client_create(config, &client) != GALAY_OK) {
        exit_code = 1;
        goto cleanup;
    }

    state.server = server;
    state.client = client;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
        exit_code = 1;
        goto cleanup;
    }
    if (galay_coro_spawn(&runtime, server_entry, &state, NULL, &server_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &state, NULL, &client_task).code != C_IOResultOk ||
        galay_coro_join(&client_task, join_timeout_ms).code != C_IOResultOk ||
        galay_coro_join(&server_task, join_timeout_ms).code != C_IOResultOk ||
        state.failed != 0 ||
        state.calls != state.requests - 1) {
        exit_code = 1;
        goto cleanup;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &finished) != 0) {
        exit_code = 1;
        goto cleanup;
    }
    const double seconds = elapsed_seconds(started, finished);
    if (seconds <= 0.0 ||
        printf("mcp http_post_keepalive requests=%d tool_calls=%d seconds=%.6f throughput=%.2f req/s\n",
               state.requests,
               state.calls,
               seconds,
               (double)state.requests / seconds) < 0) {
        exit_code = 1;
    }

cleanup:
    if (server_task.task != NULL && galay_coro_destroy(&server_task).code != C_IOResultOk && exit_code == 0) {
        exit_code = 1;
    }
    if (client_task.task != NULL && galay_coro_destroy(&client_task).code != C_IOResultOk && exit_code == 0) {
        exit_code = 1;
    }
    if (server != NULL && galay_mcp_http_server_stop(server).code != C_IOResultOk && exit_code == 0) {
        exit_code = 1;
    }
    galay_mcp_client_destroy(client);
    galay_mcp_client_config_destroy(config);
    galay_mcp_server_destroy(server);
    if (runtime.runtime != NULL && galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
        exit_code = 1;
    }
    if (runtime.runtime != NULL && galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
        exit_code = 1;
    }
    return exit_code;
}
