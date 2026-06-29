#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-mcp-c/mcp.h>

#include <stdio.h>
#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

typedef struct HttpLoopbackState {
    galay_mcp_server_t* server;
    galay_mcp_client_t* client;
    C_IOResult serve_results[4];
    C_IOResult connect_result;
    C_IOResult initialize_result;
    C_IOResult ping_result;
    C_IOResult list_result;
    C_IOResult call_result;
    C_IOResult disconnect_result;
    galay_mcp_message_t* list_tools;
    galay_mcp_message_t* call_tool;
    int call_count;
} HttpLoopbackState;

typedef struct HttpAuthState {
    galay_mcp_server_t* server;
    galay_mcp_client_t* client;
    C_IOResult serve_result;
    C_IOResult connect_result;
    C_IOResult initialize_result;
} HttpAuthState;

static galay_status_t http_tool(const char* arguments,
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
        "{\"content\":[{\"type\":\"text\",\"text\":\"http-echo\"}],\"isError\":false}");
}

static void http_server_entry(void* arg)
{
    HttpLoopbackState* state = (HttpLoopbackState*)arg;
    for (size_t index = 0; index < 4; ++index) {
        state->serve_results[index] = galay_mcp_http_server_serve_once(state->server, 1000);
        if (state->serve_results[index].code != C_IOResultOk) {
            return;
        }
    }
}

static void http_client_entry(void* arg)
{
    HttpLoopbackState* state = (HttpLoopbackState*)arg;
    state->connect_result = galay_mcp_client_connect_async(state->client, 1000);
    if (state->connect_result.code != C_IOResultOk) {
        return;
    }
    state->initialize_result = galay_mcp_client_initialize_async(state->client, "c-http-test", "1.0.0", 1000);
    state->ping_result = galay_mcp_client_ping_async(state->client, 1000);
    state->list_result = galay_mcp_client_list_tools_async(state->client, 1000, &state->list_tools);
    state->call_result =
        galay_mcp_client_call_tool_async(state->client, "echo", "{\"value\":11}", 1000, &state->call_tool);
    state->disconnect_result = galay_mcp_client_disconnect_async(state->client, 1000);
}

static void http_auth_server_entry(void* arg)
{
    HttpAuthState* state = (HttpAuthState*)arg;
    state->serve_result = galay_mcp_http_server_serve_once(state->server, 1000);
}

static void http_auth_client_entry(void* arg)
{
    HttpAuthState* state = (HttpAuthState*)arg;
    state->connect_result = galay_mcp_client_connect_async(state->client, 1000);
    if (state->connect_result.code != C_IOResultOk) {
        return;
    }
    state->initialize_result =
        galay_mcp_client_initialize_async(state->client, "auth-test", "1.0.0", 1000);
}

static int run_http_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    galay_mcp_server_t* server = NULL;
    galay_mcp_client_config_t* config = NULL;
    galay_mcp_client_t* client = NULL;
    HttpLoopbackState state = {0};
    const char* host = NULL;
    uint16_t port = 0;
    char url[128];
    const char* data = NULL;
    size_t data_len = 0;
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(galay_mcp_http_server_create("127.0.0.1", 0, &server) == GALAY_OK, 3);
    REQUIRE_TRUE(galay_mcp_server_set_info(server, "c-http-loopback", "1.0.0") == GALAY_OK, 4);
    REQUIRE_TRUE(galay_mcp_server_add_tool(server,
                                           "echo",
                                           "echo test tool",
                                           "{\"type\":\"object\"}",
                                           http_tool,
                                           &state.call_count) == GALAY_OK,
                 5);
    REQUIRE_TRUE(galay_mcp_http_server_start(server) == GALAY_OK, 6);
    REQUIRE_TRUE(galay_mcp_http_server_endpoint(server, &host, &port) == GALAY_OK, 7);
    REQUIRE_TRUE(host != NULL && port != 0, 8);
    int written = snprintf(url, sizeof(url), "http://%s:%u/mcp", host, (unsigned)port);
    REQUIRE_TRUE(written > 0 && (size_t)written < sizeof(url), 9);
    REQUIRE_TRUE(galay_mcp_http_config_create(url, &config) == GALAY_OK, 9);
    REQUIRE_TRUE(galay_mcp_client_create(config, &client) == GALAY_OK, 10);

    state.server = server;
    state.client = client;
    REQUIRE_TRUE(galay_coro_spawn(&runtime, http_server_entry, &state, NULL, &server_task).code ==
                     C_IOResultOk,
                 11);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, http_client_entry, &state, NULL, &client_task).code ==
                     C_IOResultOk,
                 12);
    REQUIRE_TRUE(galay_coro_join(&client_task, 3000).code == C_IOResultOk, 13);
    REQUIRE_TRUE(galay_coro_join(&server_task, 3000).code == C_IOResultOk, 14);

    if (state.connect_result.code != C_IOResultOk ||
        state.initialize_result.code != C_IOResultOk ||
        state.ping_result.code != C_IOResultOk ||
        state.list_result.code != C_IOResultOk ||
        state.call_result.code != C_IOResultOk ||
        state.disconnect_result.code != C_IOResultOk ||
        state.call_count != 1 ||
        state.list_tools == NULL ||
        state.call_tool == NULL ||
        galay_mcp_message_data(state.list_tools, &data, &data_len) != GALAY_OK ||
        data == NULL ||
        strstr(data, "\"echo\"") == NULL ||
        galay_mcp_message_data(state.call_tool, &data, &data_len) != GALAY_OK ||
        data == NULL ||
        strstr(data, "http-echo") == NULL) {
        result = 15;
    }

    if (server_task.task != NULL && galay_coro_destroy(&server_task).code != C_IOResultOk && result == 0) {
        result = 16;
    }
    if (client_task.task != NULL && galay_coro_destroy(&client_task).code != C_IOResultOk && result == 0) {
        result = 17;
    }
    if (galay_mcp_http_server_stop(server).code != C_IOResultOk && result == 0) {
        result = 18;
    }
    galay_mcp_message_destroy(state.call_tool);
    galay_mcp_message_destroy(state.list_tools);
    galay_mcp_client_destroy(client);
    galay_mcp_client_config_destroy(config);
    galay_mcp_server_destroy(server);
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 19;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 20;
    }
    return result;
}

static int run_http_auth_failure(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    galay_mcp_server_t* server = NULL;
    galay_mcp_client_config_t* config = NULL;
    galay_mcp_client_t* client = NULL;
    HttpAuthState state = {0};
    const char* host = NULL;
    uint16_t port = 0;
    char url[128];
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 21);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 22);
    REQUIRE_TRUE(galay_mcp_http_server_create("127.0.0.1", 0, &server) == GALAY_OK, 23);
    REQUIRE_TRUE(galay_mcp_http_server_require_bearer_token(server, "secret") == GALAY_OK, 24);
    REQUIRE_TRUE(galay_mcp_http_server_start(server) == GALAY_OK, 25);
    REQUIRE_TRUE(galay_mcp_http_server_endpoint(server, &host, &port) == GALAY_OK, 26);
    int written = snprintf(url, sizeof(url), "http://%s:%u/mcp", host, (unsigned)port);
    REQUIRE_TRUE(written > 0 && (size_t)written < sizeof(url), 27);
    REQUIRE_TRUE(galay_mcp_http_config_create(url, &config) == GALAY_OK, 28);
    REQUIRE_TRUE(galay_mcp_http_config_set_bearer_token(config, "wrong") == GALAY_OK, 29);
    REQUIRE_TRUE(galay_mcp_client_create(config, &client) == GALAY_OK, 30);

    state.server = server;
    state.client = client;
    REQUIRE_TRUE(galay_coro_spawn(&runtime, http_auth_server_entry, &state, NULL, &server_task).code ==
                     C_IOResultOk,
                 31);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, http_auth_client_entry, &state, NULL, &client_task).code ==
                     C_IOResultOk,
                 32);
    REQUIRE_TRUE(galay_coro_join(&client_task, 3000).code == C_IOResultOk, 33);
    REQUIRE_TRUE(galay_coro_join(&server_task, 3000).code == C_IOResultOk, 34);

    if (state.serve_result.code != C_IOResultOk ||
        state.connect_result.code != C_IOResultOk ||
        state.initialize_result.code == C_IOResultOk) {
        result = 35;
    }

    if (server_task.task != NULL && galay_coro_destroy(&server_task).code != C_IOResultOk && result == 0) {
        result = 36;
    }
    if (client_task.task != NULL && galay_coro_destroy(&client_task).code != C_IOResultOk && result == 0) {
        result = 37;
    }
    if (galay_mcp_http_server_stop(server).code != C_IOResultOk && result == 0) {
        result = 38;
    }
    galay_mcp_client_destroy(client);
    galay_mcp_client_config_destroy(config);
    galay_mcp_server_destroy(server);
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 39;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 40;
    }
    return result;
}

int main(void)
{
    int result = run_http_loopback();
    if (result != 0) {
        return result;
    }
    return run_http_auth_failure();
}
