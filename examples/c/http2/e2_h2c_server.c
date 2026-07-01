#include <galay/c/galay-http2-c/http2_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>
#include <string.h>

typedef struct ServerExampleState {
    galay_http2_server_t* server;
    uint16_t port;
    C_IOResult server_result;
    C_IOResult client_result;
    char body[32];
    size_t body_len;
} ServerExampleState;

static int add_header(galay_http2_headers_t* headers, const char* name, const char* value)
{
    return galay_http2_headers_add(headers, name, value) == GALAY_OK ? 0 : 1;
}

static void server_entry(void* arg)
{
    static const char response[] = "h2c-server-example";
    ServerExampleState* state = (ServerExampleState*)arg;
    galay_http2_conn_t* conn = 0;
    galay_http2_stream_t* stream = 0;
    galay_http2_headers_t* request_headers = 0;
    galay_http2_headers_t* response_headers = 0;

    state->server_result = galay_http2_server_accept(state->server, &conn, 2000);
    if (state->server_result.code != C_IOResultOk) {
        return;
    }
    state->server_result = galay_http2_conn_accept_stream(conn, &stream, 2000);
    if (state->server_result.code == C_IOResultOk &&
        galay_http2_stream_read_headers(stream, &request_headers, 0).code == C_IOResultOk &&
        galay_http2_headers_create(&response_headers) == GALAY_OK &&
        add_header(response_headers, ":status", "200") == 0) {
        state->server_result =
            galay_http2_stream_write_headers(stream, response_headers, GALAY_FALSE, 2000);
        if (state->server_result.code == C_IOResultOk) {
            state->server_result =
                galay_http2_stream_write_data(stream,
                                             response,
                                             sizeof(response) - 1,
                                             GALAY_TRUE,
                                             2000);
        }
    }
    if (request_headers != 0) {
        galay_http2_headers_destroy(request_headers);
    }
    if (response_headers != 0) {
        galay_http2_headers_destroy(response_headers);
    }
    if (stream != 0) {
        if (galay_http2_stream_destroy(stream) != GALAY_OK &&
            state->server_result.code == C_IOResultOk) {
            state->server_result = (C_IOResult){C_IOResultError, 0, 0,
                                                GALAY_HTTP2_ERROR_INTERNAL, 0};
        }
    }
    if (conn != 0) {
        if (galay_http2_conn_destroy(conn) != GALAY_OK &&
            state->server_result.code == C_IOResultOk) {
            state->server_result = (C_IOResult){C_IOResultError, 0, 0,
                                                GALAY_HTTP2_ERROR_INTERNAL, 0};
        }
    }
}

static void client_entry(void* arg)
{
    ServerExampleState* state = (ServerExampleState*)arg;
    galay_http2_client_t* client = 0;
    galay_http2_stream_t* stream = 0;
    galay_http2_headers_t* request_headers = 0;
    galay_http2_headers_t* response_headers = 0;
    galay_bool_t end_stream = GALAY_FALSE;
    galay_http2_config_t config = galay_http2_config_default();
    config.host = "127.0.0.1";
    config.port = state->port;

    if (galay_http2_headers_create(&request_headers) != GALAY_OK ||
        add_header(request_headers, ":method", "GET") != 0 ||
        add_header(request_headers, ":scheme", "http") != 0 ||
        add_header(request_headers, ":authority", "127.0.0.1") != 0 ||
        add_header(request_headers, ":path", "/server") != 0 ||
        galay_http2_client_create(&config, &client) != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0,
                                            GALAY_HTTP2_ERROR_INTERNAL, 0};
        goto cleanup;
    }
    state->client_result = galay_http2_client_connect(client, 2000);
    if (state->client_result.code == C_IOResultOk) {
        state->client_result =
            galay_http2_client_open_stream(client, request_headers, GALAY_TRUE, &stream, 2000);
    }
    if (state->client_result.code == C_IOResultOk &&
        galay_http2_stream_read_headers(stream, &response_headers, 2000).code == C_IOResultOk) {
        state->client_result =
            galay_http2_stream_read_data(stream,
                                        state->body,
                                        sizeof(state->body),
                                        &state->body_len,
                                        &end_stream,
                                        2000);
    }

cleanup:
    if (response_headers != 0) {
        galay_http2_headers_destroy(response_headers);
    }
    if (stream != 0) {
        if (galay_http2_stream_destroy(stream) != GALAY_OK &&
            state->client_result.code == C_IOResultOk) {
            state->client_result = (C_IOResult){C_IOResultError, 0, 0,
                                                GALAY_HTTP2_ERROR_INTERNAL, 0};
        }
    }
    if (request_headers != 0) {
        galay_http2_headers_destroy(request_headers);
    }
    if (client != 0) {
        galay_http2_client_destroy(client);
    }
}

int main(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;
    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    ServerExampleState state = {0};
    int exit_code = 0;

    galay_http2_config_t server_config = galay_http2_config_default();
    server_config.host = "127.0.0.1";

    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_http2_server_create(&server_config, &state.server) != GALAY_OK ||
        galay_http2_server_listen(state.server, &state.port).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, server_entry, &state, 0, &server_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &state, 0, &client_task).code != C_IOResultOk ||
        galay_coro_join(&server_task, 3000).code != C_IOResultOk ||
        galay_coro_join(&client_task, 3000).code != C_IOResultOk ||
        state.server_result.code != C_IOResultOk ||
        state.client_result.code != C_IOResultOk) {
        exit_code = 1;
    } else if (printf("h2c_server served=%.*s port=%u\n",
                      (int)state.body_len,
                      state.body,
                      state.port) < 0) {
        exit_code = 2;
    }

    if (server_task.task != 0 && galay_coro_destroy(&server_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 3;
    }
    if (client_task.task != 0 && galay_coro_destroy(&client_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 4;
    }
    if (state.server != 0) {
        if (galay_http2_server_stop(state.server, 1000).code != C_IOResultOk && exit_code == 0) {
            exit_code = 5;
        }
        galay_http2_server_destroy(state.server);
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 6;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 7;
        }
    }
    return exit_code;
}
