#include <galay/c/galay-http-c/http.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>
#include <string.h>

typedef struct LoopbackState {
    galay_http_server_t* server;
    galay_http_client_t* client;
    C_Host endpoint;
    C_IOResult server_result;
    C_IOResult client_result;
    int callback_count;
    int callback_checked_request;
    int client_checked_response;
} LoopbackState;

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "status mismatch: got %d expected %d\n", (int)actual, (int)expected);
        return 1;
    }
    return 0;
}

static int expect_io(C_IOResult actual, C_IOResultCode expected)
{
    if (actual.code != expected) {
        fprintf(stderr, "io mismatch: got %d expected %d errno=%d bytes=%zu\n",
                (int)actual.code, (int)expected, actual.sys_errno, actual.bytes);
        return 1;
    }
    return 0;
}

static int assign_loopback(C_Host* host)
{
    if (host == NULL) {
        return 1;
    }
    host->type = C_IPTypeIPV4;
    int written = snprintf(host->address, sizeof(host->address), "%s", "127.0.0.1");
    if (written <= 0 || (size_t)written >= sizeof(host->address)) {
        return 1;
    }
    host->port = 0;
    return 0;
}

static int string_equals(const char* value, size_t value_len, const char* expected)
{
    return value != NULL && value_len == strlen(expected) &&
           strncmp(value, expected, value_len) == 0;
}

static galay_status_t ping_route(const galay_http_request_t* request,
                                 galay_http_response_t* response,
                                 void* user_data)
{
    LoopbackState* state = (LoopbackState*)user_data;
    const char* path = NULL;
    size_t path_len = 0;
    galay_http_method_t method = (galay_http_method_t)99;
    const char* body = NULL;
    size_t body_len = 0;

    if (state == NULL) {
        return GALAY_INVALID_ARGUMENT;
    }
    state->callback_count += 1;
    if (galay_http_request_method(request, &method) != GALAY_OK ||
        galay_http_request_path(request, &path, &path_len) != GALAY_OK ||
        galay_http_request_body(request, &body, &body_len) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (method == GALAY_HTTP_METHOD_GET &&
        string_equals(path, path_len, "/ping") &&
        string_equals(body, body_len, "hello")) {
        state->callback_checked_request = 1;
    }
    if (galay_http_response_set_status(response, GALAY_HTTP_STATUS_OK) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    return galay_http_response_set_body(response, "pong", strlen("pong"));
}

static void server_entry(void* arg)
{
    LoopbackState* state = (LoopbackState*)arg;
    state->server_result = galay_http_server_serve_one(state->server, 2000);
}

static void client_entry(void* arg)
{
    LoopbackState* state = (LoopbackState*)arg;
    galay_http_request_t* request = NULL;
    galay_http_response_t* response = NULL;
    const char* body = NULL;
    size_t body_len = 0;
    galay_http_status_code_t status = (galay_http_status_code_t)0;

    if (galay_http_request_create(&request) != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    if (galay_http_request_set_method_path(request, GALAY_HTTP_METHOD_GET, "/ping") != GALAY_OK ||
        galay_http_request_set_body(request, "hello", strlen("hello")) != GALAY_OK ||
        galay_http_request_add_header(request, "Host", "127.0.0.1") != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        galay_http_request_destroy(request);
        return;
    }

    state->client_result = galay_http_client_connect(state->client, &state->endpoint, 2000);
    if (state->client_result.code != C_IOResultOk) {
        galay_http_request_destroy(request);
        return;
    }
    state->client_result = galay_http_client_send_request(state->client, request, 2000);
    if (state->client_result.code != C_IOResultOk) {
        galay_http_request_destroy(request);
        return;
    }
    state->client_result =
        galay_http_client_recv_response(state->client, &response, 4096, 4096, 2000);
    if (state->client_result.code == C_IOResultOk &&
        galay_http_response_status(response, &status) == GALAY_OK &&
        galay_http_response_body(response, &body, &body_len) == GALAY_OK &&
        status == GALAY_HTTP_STATUS_OK &&
        string_equals(body, body_len, "pong")) {
        state->client_checked_response = 1;
    }
    if (response != NULL) {
        galay_http_response_destroy(response);
    }
    galay_http_request_destroy(request);
}

static int cleanup(galay_kernel_runtime_t* runtime,
                   galay_coro_task_t* server_task,
                   galay_coro_task_t* client_task,
                   LoopbackState* state,
                   int exit_code)
{
    if (server_task->task != NULL) {
        C_IOResult destroy_result = galay_coro_destroy(server_task);
        if (destroy_result.code != C_IOResultOk && exit_code == 0) {
            exit_code = 90;
        }
    }
    if (client_task->task != NULL) {
        C_IOResult destroy_result = galay_coro_destroy(client_task);
        if (destroy_result.code != C_IOResultOk && exit_code == 0) {
            exit_code = 91;
        }
    }
    if (state->client != NULL) {
        C_IOResult close_result = galay_http_client_close(state->client, 2000);
        if (close_result.code != C_IOResultOk &&
            close_result.code != C_IOResultInvalid &&
            exit_code == 0) {
            exit_code = 92;
        }
        if (galay_http_client_destroy(state->client) != GALAY_OK && exit_code == 0) {
            exit_code = 93;
        }
        state->client = NULL;
    }
    if (state->server != NULL) {
        C_IOResult stop_result = galay_http_server_stop(state->server, 2000);
        if (stop_result.code != C_IOResultOk &&
            stop_result.code != C_IOResultInvalid &&
            exit_code == 0) {
            exit_code = 94;
        }
        if (galay_http_server_destroy(state->server) != GALAY_OK && exit_code == 0) {
            exit_code = 95;
        }
        state->server = NULL;
    }
    if (runtime->runtime != NULL) {
        if (galay_kernel_runtime_stop(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 96;
        }
        if (galay_kernel_runtime_destroy(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 97;
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
    LoopbackState state = {0};

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return cleanup(&runtime, &server_task, &client_task, &state, 2);
    }
    if (expect_status(galay_http_server_create(&state.server), GALAY_OK) ||
        expect_status(galay_http_client_create(&state.client), GALAY_OK)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 3);
    }
    if (assign_loopback(&state.endpoint) != 0 ||
        expect_status(galay_http_server_bind(state.server, &state.endpoint), GALAY_OK) ||
        expect_status(galay_http_server_listen(state.server, 16), GALAY_OK) ||
        expect_status(galay_http_server_local_endpoint(state.server, &state.endpoint), GALAY_OK)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 4);
    }
    if (expect_status(galay_http_server_add_route(state.server,
                                                  GALAY_HTTP_METHOD_GET,
                                                  "/ping",
                                                  ping_route,
                                                  &state),
                      GALAY_OK)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 5);
    }

    if (expect_io(galay_coro_spawn(&runtime, server_entry, &state, NULL, &server_task),
                  C_IOResultOk)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 6);
    }
    if (expect_io(galay_coro_spawn(&runtime, client_entry, &state, NULL, &client_task),
                  C_IOResultOk)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 7);
    }
    if (expect_io(galay_coro_join(&client_task, 3000), C_IOResultOk) ||
        expect_io(galay_coro_join(&server_task, 3000), C_IOResultOk)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 8);
    }
    if (expect_io(state.client_result, C_IOResultOk) ||
        expect_io(state.server_result, C_IOResultOk) ||
        state.callback_count != 1 ||
        state.callback_checked_request != 1 ||
        state.client_checked_response != 1) {
        return cleanup(&runtime, &server_task, &client_task, &state, 9);
    }

    return cleanup(&runtime, &server_task, &client_task, &state, 0);
}
