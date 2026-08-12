#include <galay/c/galay-http2-c/http2_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdint.h>
#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

typedef struct FlowControlState {
    galay_http2_server_t* server;
    uint16_t port;
    galay_http2_client_t* client;
    galay_http2_conn_t* server_conn;
    galay_http2_stream_t* server_stream;
    galay_http2_stream_t* client_stream;
    C_IOResult server_accept_result;
    C_IOResult server_stream_result;
    C_IOResult server_read_first_result;
    C_IOResult server_window_update_result;
    C_IOResult server_read_second_result;
    C_IOResult client_connect_result;
    C_IOResult client_open_result;
    C_IOResult client_oversize_write_result;
    C_IOResult client_first_write_result;
    C_IOResult client_control_result;
    C_IOResult client_second_write_result;
    char first[16];
    char second[16];
    size_t first_len;
    size_t second_len;
    galay_bool_t first_end;
    galay_bool_t second_end;
} FlowControlState;

static int make_headers(galay_http2_headers_t** out)
{
    if (galay_http2_headers_create(out) != GALAY_OK) {
        return 1;
    }
    if (galay_http2_headers_add(*out, ":method", "POST") != GALAY_OK ||
        galay_http2_headers_add(*out, ":scheme", "http") != GALAY_OK ||
        galay_http2_headers_add(*out, ":authority", "127.0.0.1") != GALAY_OK ||
        galay_http2_headers_add(*out, ":path", "/flow") != GALAY_OK) {
        galay_http2_headers_destroy(*out);
        *out = 0;
        return 1;
    }
    return 0;
}

static void flow_server_entry(void* arg)
{
    FlowControlState* state = (FlowControlState*)arg;
    state->server_accept_result =
        galay_http2_server_accept(state->server, &state->server_conn, 2000);
    if (state->server_accept_result.code != C_IOResultOk) {
        return;
    }
    state->server_stream_result =
        galay_http2_conn_accept_stream(state->server_conn, &state->server_stream, 2000);
    if (state->server_stream_result.code != C_IOResultOk) {
        return;
    }
    state->server_read_first_result =
        galay_http2_stream_read_data(state->server_stream,
                                     state->first,
                                     sizeof(state->first),
                                     &state->first_len,
                                     &state->first_end,
                                     2000);
    if (state->server_read_first_result.code != C_IOResultOk) {
        return;
    }
    state->server_window_update_result =
        galay_http2_conn_send_window_update(state->server_conn, state->server_stream, 8, 2000);
    if (state->server_window_update_result.code != C_IOResultOk) {
        return;
    }
    state->server_read_second_result =
        galay_http2_stream_read_data(state->server_stream,
                                     state->second,
                                     sizeof(state->second),
                                     &state->second_len,
                                     &state->second_end,
                                     2000);
}

static void flow_client_entry(void* arg)
{
    FlowControlState* state = (FlowControlState*)arg;
    galay_http2_config_t config = galay_http2_config_default();
    galay_http2_headers_t* headers = 0;
    static const char oversize[] = "123456789";
    static const char first[] = "12345678";
    static const char second[] = "WXYZ";

    config.host = "127.0.0.1";
    config.port = state->port;
    config.initial_window_size = 8;
    if (galay_http2_client_create(&config, &state->client) != GALAY_OK ||
        make_headers(&headers) != 0) {
        state->client_connect_result = (C_IOResult){C_IOResultError, 0, 0,
                                                    GALAY_HTTP2_ERROR_INTERNAL, 0};
        goto cleanup;
    }
    state->client_connect_result = galay_http2_client_connect(state->client, 2000);
    if (state->client_connect_result.code != C_IOResultOk) {
        goto cleanup;
    }
    state->client_open_result =
        galay_http2_client_open_stream(state->client, headers, GALAY_FALSE,
                                       &state->client_stream, 2000);
    if (state->client_open_result.code != C_IOResultOk) {
        goto cleanup;
    }

    state->client_oversize_write_result =
        galay_http2_stream_write_data(state->client_stream, oversize, sizeof(oversize) - 1,
                                      GALAY_FALSE, 2000);
    state->client_first_write_result =
        galay_http2_stream_write_data(state->client_stream, first, sizeof(first) - 1,
                                      GALAY_FALSE, 2000);
    state->client_control_result =
        galay_http2_conn_read_control(galay_http2_client_conn(state->client), 2000);
    state->client_second_write_result =
        galay_http2_stream_write_data(state->client_stream, second, sizeof(second) - 1,
                                      GALAY_TRUE, 2000);

cleanup:
    if (headers != 0) {
        galay_http2_headers_destroy(headers);
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
    FlowControlState state = {0};
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);

    galay_http2_config_t server_config = galay_http2_config_default();
    server_config.host = "127.0.0.1";
    server_config.port = 0;
    server_config.initial_window_size = 8;
    REQUIRE_TRUE(galay_http2_server_create(&server_config, &state.server) == GALAY_OK, 3);
    REQUIRE_TRUE(galay_http2_server_listen(state.server, &state.port).code == C_IOResultOk, 4);

    REQUIRE_TRUE(galay_coro_spawn(&runtime, flow_server_entry, &state, 0, &server_task).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, flow_client_entry, &state, 0, &client_task).code ==
                     C_IOResultOk,
                 6);
    REQUIRE_TRUE(galay_coro_join(&server_task, 3000).code == C_IOResultOk, 7);
    REQUIRE_TRUE(galay_coro_join(&client_task, 3000).code == C_IOResultOk, 8);

    if (state.server_accept_result.code != C_IOResultOk ||
        state.server_stream_result.code != C_IOResultOk ||
        state.server_read_first_result.code != C_IOResultOk ||
        state.server_window_update_result.code != C_IOResultOk ||
        state.server_read_second_result.code != C_IOResultOk ||
        state.client_connect_result.code != C_IOResultOk ||
        state.client_open_result.code != C_IOResultOk ||
        state.client_oversize_write_result.code != C_IOResultError ||
        state.client_oversize_write_result.value != GALAY_HTTP2_ERROR_FLOW_CONTROL ||
        state.client_first_write_result.code != C_IOResultOk ||
        state.client_control_result.code != C_IOResultOk ||
        state.client_second_write_result.code != C_IOResultOk ||
        state.first_len != 8 ||
        state.second_len != 4 ||
        state.first_end != GALAY_FALSE ||
        state.second_end != GALAY_TRUE ||
        memcmp(state.first, "12345678", state.first_len) != 0 ||
        memcmp(state.second, "WXYZ", state.second_len) != 0) {
        result = 9;
    }

    if (state.client_stream != 0 &&
        galay_http2_stream_destroy(state.client_stream) != GALAY_OK && result == 0) {
        result = 10;
    }
    if (state.server_stream != 0 &&
        galay_http2_stream_destroy(state.server_stream) != GALAY_OK && result == 0) {
        result = 11;
    }
    if (state.server_conn != 0 && galay_http2_conn_destroy(state.server_conn) != GALAY_OK &&
        result == 0) {
        result = 12;
    }
    if (state.client != 0) {
        galay_http2_client_destroy(state.client);
    }
    if (state.server != 0) {
        C_IOResult stopped = galay_http2_server_stop(state.server, 1000);
        if (stopped.code != C_IOResultOk && result == 0) {
            result = 13;
        }
        galay_http2_server_destroy(state.server);
    }
    if (server_task.task != 0 && galay_coro_destroy(&server_task).code != C_IOResultOk &&
        result == 0) {
        result = 14;
    }
    if (client_task.task != 0 && galay_coro_destroy(&client_task).code != C_IOResultOk &&
        result == 0) {
        result = 15;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 16;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 17;
    }
    return result;
}
