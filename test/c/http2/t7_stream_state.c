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

typedef struct StreamStateTestState {
    galay_http2_server_t* server;
    uint16_t port;
    galay_http2_client_t* client;
    galay_http2_conn_t* server_conn;
    galay_http2_stream_t* server_stream;
    galay_http2_stream_t* client_stream;
    C_IOResult server_accept_result;
    C_IOResult server_stream_result;
    C_IOResult server_reset_read_result;
    C_IOResult server_goaway_result;
    C_IOResult client_connect_result;
    C_IOResult client_open_result;
    C_IOResult client_reset_result;
    C_IOResult client_invalid_write_result;
    C_IOResult client_control_result;
    C_IOResult client_goaway_open_result;
    galay_bool_t server_settings_ack;
    galay_bool_t client_settings_ack;
} StreamStateTestState;

static int make_headers(galay_http2_headers_t** out)
{
    if (galay_http2_headers_create(out) != GALAY_OK) {
        return 1;
    }
    if (galay_http2_headers_add(*out, ":method", "GET") != GALAY_OK ||
        galay_http2_headers_add(*out, ":scheme", "http") != GALAY_OK ||
        galay_http2_headers_add(*out, ":authority", "127.0.0.1") != GALAY_OK ||
        galay_http2_headers_add(*out, ":path", "/reset") != GALAY_OK) {
        galay_http2_headers_destroy(*out);
        *out = 0;
        return 1;
    }
    return 0;
}

static void state_server_entry(void* arg)
{
    StreamStateTestState* state = (StreamStateTestState*)arg;
    char buffer[8];
    size_t read_len = 0;
    galay_bool_t end_stream = GALAY_FALSE;

    state->server_accept_result =
        galay_http2_server_accept(state->server, &state->server_conn, 2000);
    if (state->server_accept_result.code != C_IOResultOk) {
        return;
    }
    state->server_settings_ack = galay_http2_conn_settings_ack_received(state->server_conn);
    state->server_stream_result =
        galay_http2_conn_accept_stream(state->server_conn, &state->server_stream, 2000);
    if (state->server_stream_result.code != C_IOResultOk) {
        return;
    }
    state->server_reset_read_result =
        galay_http2_stream_read_data(state->server_stream,
                                     buffer,
                                     sizeof(buffer),
                                     &read_len,
                                     &end_stream,
                                     2000);
    state->server_goaway_result =
        galay_http2_conn_send_goaway(state->server_conn, 1, GALAY_HTTP2_ERROR_NONE, 2000);
}

static void state_client_entry(void* arg)
{
    StreamStateTestState* state = (StreamStateTestState*)arg;
    galay_http2_config_t config = galay_http2_config_default();
    galay_http2_headers_t* headers = 0;
    static const char data[] = "x";

    config.host = "127.0.0.1";
    config.port = state->port;
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
    state->client_settings_ack =
        galay_http2_conn_settings_ack_received(galay_http2_client_conn(state->client));
    state->client_open_result =
        galay_http2_client_open_stream(state->client, headers, GALAY_FALSE,
                                       &state->client_stream, 2000);
    if (state->client_open_result.code != C_IOResultOk) {
        goto cleanup;
    }
    state->client_reset_result =
        galay_http2_stream_reset(state->client_stream, GALAY_HTTP2_ERROR_CANCEL, 2000);
    state->client_invalid_write_result =
        galay_http2_stream_write_data(state->client_stream, data, sizeof(data) - 1, GALAY_TRUE, 2000);
    state->client_control_result =
        galay_http2_conn_read_control(galay_http2_client_conn(state->client), 2000);
    state->client_goaway_open_result =
        galay_http2_client_open_stream(state->client, headers, GALAY_TRUE, 0, 2000);

cleanup:
    if (headers != 0) {
        galay_http2_headers_destroy(headers);
    }
}

static int settings_ack_with_payload_is_rejected(void)
{
    uint8_t settings_ack_with_payload[GALAY_HTTP2_FRAME_HEADER_LENGTH + 6] = {
        0, 0, 6,
        GALAY_HTTP2_FRAME_SETTINGS,
        0x1,
        0, 0, 0, 0,
        0, GALAY_HTTP2_SETTINGS_ENABLE_PUSH, 0, 0, 0, 1,
    };
    galay_http2_frame_t* frame = 0;
    galay_status_t status = galay_http2_frame_decode(settings_ack_with_payload,
                                                     sizeof(settings_ack_with_payload),
                                                     &frame);
    if (frame != 0) {
        galay_http2_frame_destroy(frame);
        return 1;
    }
    return status == GALAY_PROTOCOL_ERROR ? 0 : 1;
}

int main(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    StreamStateTestState state = {0};
    int result = 0;

    REQUIRE_TRUE(settings_ack_with_payload_is_rejected() == 0, 1);
    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 3);

    galay_http2_config_t server_config = galay_http2_config_default();
    server_config.host = "127.0.0.1";
    server_config.port = 0;
    REQUIRE_TRUE(galay_http2_server_create(&server_config, &state.server) == GALAY_OK, 4);
    REQUIRE_TRUE(galay_http2_server_listen(state.server, &state.port).code == C_IOResultOk, 5);

    REQUIRE_TRUE(galay_coro_spawn(&runtime, state_server_entry, &state, 0, &server_task).code ==
                     C_IOResultOk,
                 6);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, state_client_entry, &state, 0, &client_task).code ==
                     C_IOResultOk,
                 7);
    REQUIRE_TRUE(galay_coro_join(&server_task, 3000).code == C_IOResultOk, 8);
    REQUIRE_TRUE(galay_coro_join(&client_task, 3000).code == C_IOResultOk, 9);

    if (state.server_accept_result.code != C_IOResultOk ||
        state.server_stream_result.code != C_IOResultOk ||
        state.server_reset_read_result.code != C_IOResultError ||
        state.server_reset_read_result.value != GALAY_HTTP2_ERROR_STREAM_RESET ||
        state.server_goaway_result.code != C_IOResultOk ||
        state.client_connect_result.code != C_IOResultOk ||
        state.client_open_result.code != C_IOResultOk ||
        state.client_reset_result.code != C_IOResultOk ||
        state.client_invalid_write_result.code != C_IOResultError ||
        state.client_invalid_write_result.value != GALAY_HTTP2_ERROR_STREAM_CLOSED ||
        state.client_control_result.code != C_IOResultOk ||
        state.client_goaway_open_result.code != C_IOResultError ||
        state.client_goaway_open_result.value != GALAY_HTTP2_ERROR_GOAWAY ||
        state.server_settings_ack != GALAY_TRUE ||
        state.client_settings_ack != GALAY_TRUE ||
        galay_http2_stream_state(state.client_stream) != GALAY_HTTP2_STREAM_CLOSED ||
        galay_http2_stream_state(state.server_stream) != GALAY_HTTP2_STREAM_CLOSED) {
        result = 10;
    }

    if (state.client_stream != 0 &&
        galay_http2_stream_destroy(state.client_stream) != GALAY_OK && result == 0) {
        result = 11;
    }
    if (state.server_stream != 0 &&
        galay_http2_stream_destroy(state.server_stream) != GALAY_OK && result == 0) {
        result = 12;
    }
    if (state.server_conn != 0 && galay_http2_conn_destroy(state.server_conn) != GALAY_OK &&
        result == 0) {
        result = 13;
    }
    if (state.client != 0) {
        galay_http2_client_destroy(state.client);
    }
    if (state.server != 0) {
        C_IOResult stopped = galay_http2_server_stop(state.server, 1000);
        if (stopped.code != C_IOResultOk && result == 0) {
            result = 14;
        }
        galay_http2_server_destroy(state.server);
    }
    if (server_task.task != 0 && galay_coro_destroy(&server_task).code != C_IOResultOk &&
        result == 0) {
        result = 15;
    }
    if (client_task.task != 0 && galay_coro_destroy(&client_task).code != C_IOResultOk &&
        result == 0) {
        result = 16;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 17;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 18;
    }
    return result;
}
