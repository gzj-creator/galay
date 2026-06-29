#include <galay/c/galay-http-c/http.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>
#include <string.h>

typedef struct ExampleState {
    galay_http_server_t* server;
    galay_http_client_t* client;
    C_Host endpoint;
    C_IOResult server_result;
    C_IOResult client_result;
    char response[32];
} ExampleState;

static int assign_loopback(C_Host* host)
{
    host->type = C_IPTypeIPV4;
    int written = snprintf(host->address, sizeof(host->address), "%s", "127.0.0.1");
    if (written <= 0 || (size_t)written >= sizeof(host->address)) {
        return 1;
    }
    host->port = 0;
    return 0;
}

static galay_status_t route_handler(const galay_http_request_t* request,
                                    galay_http_response_t* response,
                                    void* user_data)
{
    ExampleState* state = (ExampleState*)user_data;
    const char* path = NULL;
    size_t path_len = 0;
    if (state == NULL ||
        galay_http_request_path(request, &path, &path_len) != GALAY_OK ||
        path_len != strlen("/hello") ||
        strncmp(path, "/hello", path_len) != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (galay_http_response_set_status(response, GALAY_HTTP_STATUS_OK) != GALAY_OK) {
        return GALAY_INTERNAL_ERROR;
    }
    return galay_http_response_set_body(response, "hello from C HTTP", strlen("hello from C HTTP"));
}

static void server_entry(void* arg)
{
    ExampleState* state = (ExampleState*)arg;
    state->server_result = galay_http_server_serve_one(state->server, 2000);
}

static void client_entry(void* arg)
{
    ExampleState* state = (ExampleState*)arg;
    galay_http_request_t* request = NULL;
    galay_http_response_t* response = NULL;
    const char* body = NULL;
    size_t body_len = 0;

    if (galay_http_request_create(&request) != GALAY_OK ||
        galay_http_request_set_method_path(request, GALAY_HTTP_METHOD_GET, "/hello") != GALAY_OK ||
        galay_http_request_add_header(request, "Host", "127.0.0.1") != GALAY_OK ||
        galay_http_request_set_body(request, NULL, 0) != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        if (request != NULL) {
            galay_http_request_destroy(request);
        }
        return;
    }

    state->client_result = galay_http_client_connect(state->client, &state->endpoint, 2000);
    if (state->client_result.code == C_IOResultOk) {
        state->client_result = galay_http_client_send_request(state->client, request, 2000);
    }
    if (state->client_result.code == C_IOResultOk) {
        state->client_result =
            galay_http_client_recv_response(state->client, &response, 4096, 4096, 2000);
    }
    if (state->client_result.code == C_IOResultOk &&
        galay_http_response_body(response, &body, &body_len) == GALAY_OK &&
        body_len < sizeof(state->response)) {
        memcpy(state->response, body, body_len);
        state->response[body_len] = '\0';
    }
    if (response != NULL) {
        galay_http_response_destroy(response);
    }
    galay_http_request_destroy(request);
}

static int cleanup(galay_kernel_runtime_t* runtime,
                   galay_coro_task_t* server_task,
                   galay_coro_task_t* client_task,
                   ExampleState* state,
                   int exit_code)
{
    if (server_task->task != NULL && galay_coro_destroy(server_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 10;
    }
    if (client_task->task != NULL && galay_coro_destroy(client_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 11;
    }
    if (state->client != NULL) {
        C_IOResult closed = galay_http_client_close(state->client, 2000);
        if (closed.code != C_IOResultOk && closed.code != C_IOResultInvalid && exit_code == 0) {
            exit_code = 12;
        }
        if (galay_http_client_destroy(state->client) != GALAY_OK && exit_code == 0) {
            exit_code = 13;
        }
    }
    if (state->server != NULL) {
        C_IOResult stopped = galay_http_server_stop(state->server, 2000);
        if (stopped.code != C_IOResultOk && stopped.code != C_IOResultInvalid && exit_code == 0) {
            exit_code = 14;
        }
        if (galay_http_server_destroy(state->server) != GALAY_OK && exit_code == 0) {
            exit_code = 15;
        }
    }
    if (runtime->runtime != NULL) {
        if (galay_kernel_runtime_stop(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 16;
        }
        if (galay_kernel_runtime_destroy(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 17;
        }
    }
    return exit_code;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    ExampleState state = {0};

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_http_server_create(&state.server) != GALAY_OK ||
        galay_http_client_create(&state.client) != GALAY_OK ||
        assign_loopback(&state.endpoint) != 0 ||
        galay_http_server_bind(state.server, &state.endpoint) != GALAY_OK ||
        galay_http_server_listen(state.server, 16) != GALAY_OK ||
        galay_http_server_local_endpoint(state.server, &state.endpoint) != GALAY_OK ||
        galay_http_server_add_route(state.server, GALAY_HTTP_METHOD_GET, "/hello",
                                    route_handler, &state) != GALAY_OK) {
        return cleanup(&runtime, &server_task, &client_task, &state, 1);
    }
    if (galay_coro_spawn(&runtime, server_entry, &state, NULL, &server_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &state, NULL, &client_task).code != C_IOResultOk ||
        galay_coro_join(&server_task, 3000).code != C_IOResultOk ||
        galay_coro_join(&client_task, 3000).code != C_IOResultOk ||
        state.server_result.code != C_IOResultOk ||
        state.client_result.code != C_IOResultOk) {
        return cleanup(&runtime, &server_task, &client_task, &state, 2);
    }
    if (printf("c_http_async_client response=\"%s\" port=%u\n",
               state.response, state.endpoint.port) < 0) {
        return cleanup(&runtime, &server_task, &client_task, &state, 3);
    }
    return cleanup(&runtime, &server_task, &client_task, &state, 0);
}
