#include <galay/c/galay-http2-c/http2_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

typedef struct H2cLoopbackState {
    galay_http2_server_t* server;
    uint16_t port;
    C_IOResult server_accept_result;
    C_IOResult server_stream_one_result;
    C_IOResult server_stream_two_result;
    C_IOResult server_write_one_result;
    C_IOResult server_write_two_result;
    C_IOResult client_connect_result;
    C_IOResult client_stream_one_result;
    C_IOResult client_stream_two_result;
    C_IOResult client_read_one_result;
    C_IOResult client_read_two_result;
    galay_http2_conn_t* server_conn;
    galay_http2_stream_t* server_stream_one;
    galay_http2_stream_t* server_stream_two;
    galay_http2_client_t* client;
    galay_http2_stream_t* client_stream_one;
    galay_http2_stream_t* client_stream_two;
    char response_one[32];
    char response_two[32];
    size_t response_one_len;
    size_t response_two_len;
    galay_bool_t response_one_end;
    galay_bool_t response_two_end;
} H2cLoopbackState;

static int add_header(galay_http2_headers_t* headers, const char* name, const char* value)
{
    return galay_http2_headers_add(headers, name, value) == GALAY_OK ? 0 : 1;
}

static int make_request_headers(const char* path, galay_http2_headers_t** out)
{
    if (galay_http2_headers_create(out) != GALAY_OK) {
        return 1;
    }
    if (add_header(*out, ":method", "GET") != 0 ||
        add_header(*out, ":scheme", "http") != 0 ||
        add_header(*out, ":authority", "127.0.0.1") != 0 ||
        add_header(*out, ":path", path) != 0) {
        galay_http2_headers_destroy(*out);
        *out = 0;
        return 1;
    }
    return 0;
}

static int make_response_headers(galay_http2_headers_t** out)
{
    if (galay_http2_headers_create(out) != GALAY_OK) {
        return 1;
    }
    if (add_header(*out, ":status", "200") != 0 ||
        add_header(*out, "content-type", "text/plain") != 0) {
        galay_http2_headers_destroy(*out);
        *out = 0;
        return 1;
    }
    return 0;
}

static int headers_path_is(galay_http2_headers_t* headers, const char* expected)
{
    for (size_t i = 0; i < galay_http2_headers_count(headers); ++i) {
        const char* name = 0;
        const char* value = 0;
        if (galay_http2_headers_get(headers, i, &name, &value) != GALAY_OK) {
            return 1;
        }
        if (strcmp(name, ":path") == 0) {
            return strcmp(value, expected) == 0 ? 0 : 1;
        }
    }
    return 1;
}

static void server_entry(void* arg)
{
    static const char one[] = "response-one";
    static const char two[] = "response-two";
    H2cLoopbackState* state = (H2cLoopbackState*)arg;
    galay_http2_headers_t* headers_one = 0;
    galay_http2_headers_t* headers_two = 0;
    galay_http2_headers_t* response_headers = 0;

    state->server_accept_result =
        galay_http2_server_accept(state->server, &state->server_conn, 2000);
    if (state->server_accept_result.code != C_IOResultOk) {
        return;
    }
    if (galay_http2_conn_settings_ack_received(state->server_conn) != GALAY_TRUE) {
        state->server_accept_result = (C_IOResult){C_IOResultError, 0, 0,
                                                   GALAY_HTTP2_ERROR_SETTINGS_ACK, 0};
        return;
    }

    state->server_stream_one_result =
        galay_http2_conn_accept_stream(state->server_conn, &state->server_stream_one, 2000);
    state->server_stream_two_result =
        galay_http2_conn_accept_stream(state->server_conn, &state->server_stream_two, 2000);
    if (state->server_stream_one_result.code != C_IOResultOk ||
        state->server_stream_two_result.code != C_IOResultOk) {
        return;
    }

    if (galay_http2_stream_read_headers(state->server_stream_one, &headers_one, 0).code !=
            C_IOResultOk ||
        galay_http2_stream_read_headers(state->server_stream_two, &headers_two, 0).code !=
            C_IOResultOk ||
        headers_path_is(headers_one, "/one") != 0 ||
        headers_path_is(headers_two, "/two") != 0 ||
        make_response_headers(&response_headers) != 0) {
        state->server_write_one_result = (C_IOResult){C_IOResultError, 0, 0,
                                                      GALAY_HTTP2_ERROR_PROTOCOL, 0};
        goto cleanup;
    }

    state->server_write_two_result =
        galay_http2_stream_write_headers(state->server_stream_two, response_headers, GALAY_FALSE, 2000);
    if (state->server_write_two_result.code == C_IOResultOk) {
        state->server_write_two_result =
            galay_http2_stream_write_data(state->server_stream_two, two, sizeof(two) - 1, GALAY_TRUE, 2000);
    }

    state->server_write_one_result =
        galay_http2_stream_write_headers(state->server_stream_one, response_headers, GALAY_FALSE, 2000);
    if (state->server_write_one_result.code == C_IOResultOk) {
        state->server_write_one_result =
            galay_http2_stream_write_data(state->server_stream_one, one, sizeof(one) - 1, GALAY_TRUE, 2000);
    }

cleanup:
    if (headers_one != 0) {
        galay_http2_headers_destroy(headers_one);
    }
    if (headers_two != 0) {
        galay_http2_headers_destroy(headers_two);
    }
    if (response_headers != 0) {
        galay_http2_headers_destroy(response_headers);
    }
}

static void client_entry(void* arg)
{
    H2cLoopbackState* state = (H2cLoopbackState*)arg;
    galay_http2_config_t config = galay_http2_config_default();
    galay_http2_headers_t* headers_one = 0;
    galay_http2_headers_t* headers_two = 0;
    galay_http2_headers_t* response_headers = 0;

    config.host = "127.0.0.1";
    config.port = state->port;
    if (galay_http2_client_create(&config, &state->client) != GALAY_OK) {
        state->client_connect_result = (C_IOResult){C_IOResultError, 0, 0,
                                                    GALAY_HTTP2_ERROR_INTERNAL, 0};
        return;
    }
    state->client_connect_result = galay_http2_client_connect(state->client, 2000);
    if (state->client_connect_result.code != C_IOResultOk) {
        return;
    }
    if (galay_http2_conn_settings_ack_received(galay_http2_client_conn(state->client)) != GALAY_TRUE) {
        state->client_connect_result = (C_IOResult){C_IOResultError, 0, 0,
                                                    GALAY_HTTP2_ERROR_SETTINGS_ACK, 0};
        return;
    }

    if (make_request_headers("/one", &headers_one) != 0 ||
        make_request_headers("/two", &headers_two) != 0) {
        state->client_stream_one_result = (C_IOResult){C_IOResultError, 0, 0,
                                                       GALAY_HTTP2_ERROR_INTERNAL, 0};
        goto cleanup;
    }

    state->client_stream_one_result =
        galay_http2_client_open_stream(state->client, headers_one, GALAY_TRUE,
                                       &state->client_stream_one, 2000);
    state->client_stream_two_result =
        galay_http2_client_open_stream(state->client, headers_two, GALAY_TRUE,
                                       &state->client_stream_two, 2000);
    if (state->client_stream_one_result.code != C_IOResultOk ||
        state->client_stream_two_result.code != C_IOResultOk) {
        goto cleanup;
    }

    if (galay_http2_stream_read_headers(state->client_stream_two, &response_headers, 2000).code !=
        C_IOResultOk) {
        state->client_read_two_result = (C_IOResult){C_IOResultError, 0, 0,
                                                     GALAY_HTTP2_ERROR_PROTOCOL, 0};
        goto cleanup;
    }
    galay_http2_headers_destroy(response_headers);
    response_headers = 0;
    state->client_read_two_result =
        galay_http2_stream_read_data(state->client_stream_two,
                                     state->response_two,
                                     sizeof(state->response_two),
                                     &state->response_two_len,
                                     &state->response_two_end,
                                     2000);

    if (galay_http2_stream_read_headers(state->client_stream_one, &response_headers, 2000).code !=
        C_IOResultOk) {
        state->client_read_one_result = (C_IOResult){C_IOResultError, 0, 0,
                                                     GALAY_HTTP2_ERROR_PROTOCOL, 0};
        goto cleanup;
    }
    galay_http2_headers_destroy(response_headers);
    response_headers = 0;
    state->client_read_one_result =
        galay_http2_stream_read_data(state->client_stream_one,
                                     state->response_one,
                                     sizeof(state->response_one),
                                     &state->response_one_len,
                                     &state->response_one_end,
                                     2000);

cleanup:
    if (headers_one != 0) {
        galay_http2_headers_destroy(headers_one);
    }
    if (headers_two != 0) {
        galay_http2_headers_destroy(headers_two);
    }
    if (response_headers != 0) {
        galay_http2_headers_destroy(response_headers);
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
    H2cLoopbackState state = {0};
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);

    galay_http2_config_t server_config = galay_http2_config_default();
    server_config.host = "127.0.0.1";
    server_config.port = 0;
    REQUIRE_TRUE(galay_http2_server_create(&server_config, &state.server) == GALAY_OK, 3);
    REQUIRE_TRUE(galay_http2_server_listen(state.server, &state.port).code == C_IOResultOk, 4);

    REQUIRE_TRUE(galay_coro_spawn(&runtime, server_entry, &state, 0, &server_task).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, client_entry, &state, 0, &client_task).code ==
                     C_IOResultOk,
                 6);
    REQUIRE_TRUE(galay_coro_join(&server_task, 3000).code == C_IOResultOk, 7);
    REQUIRE_TRUE(galay_coro_join(&client_task, 3000).code == C_IOResultOk, 8);

    if (state.server_accept_result.code != C_IOResultOk ||
        state.server_stream_one_result.code != C_IOResultOk ||
        state.server_stream_two_result.code != C_IOResultOk ||
        state.server_write_one_result.code != C_IOResultOk ||
        state.server_write_two_result.code != C_IOResultOk ||
        state.client_connect_result.code != C_IOResultOk ||
        state.client_stream_one_result.code != C_IOResultOk ||
        state.client_stream_two_result.code != C_IOResultOk ||
        state.client_read_one_result.code != C_IOResultOk ||
        state.client_read_two_result.code != C_IOResultOk ||
        state.response_one_len != strlen("response-one") ||
        state.response_two_len != strlen("response-two") ||
        state.response_one_end != GALAY_TRUE ||
        state.response_two_end != GALAY_TRUE ||
        memcmp(state.response_one, "response-one", state.response_one_len) != 0 ||
        memcmp(state.response_two, "response-two", state.response_two_len) != 0) {
        result = 9;
    }

    if (state.client_stream_one != 0 &&
        galay_http2_stream_destroy(state.client_stream_one) != GALAY_OK && result == 0) {
        result = 10;
    }
    if (state.client_stream_two != 0 &&
        galay_http2_stream_destroy(state.client_stream_two) != GALAY_OK && result == 0) {
        result = 11;
    }
    if (state.server_stream_one != 0 &&
        galay_http2_stream_destroy(state.server_stream_one) != GALAY_OK && result == 0) {
        result = 12;
    }
    if (state.server_stream_two != 0 &&
        galay_http2_stream_destroy(state.server_stream_two) != GALAY_OK && result == 0) {
        result = 13;
    }
    if (state.server_conn != 0 && galay_http2_conn_destroy(state.server_conn) != GALAY_OK &&
        result == 0) {
        result = 14;
    }
    if (state.client != 0) {
        galay_http2_client_destroy(state.client);
    }
    if (state.server != 0) {
        C_IOResult stopped = galay_http2_server_stop(state.server, 1000);
        if (stopped.code != C_IOResultOk && result == 0) {
            result = 15;
        }
        galay_http2_server_destroy(state.server);
    }
    if (server_task.task != 0 && galay_coro_destroy(&server_task).code != C_IOResultOk &&
        result == 0) {
        result = 16;
    }
    if (client_task.task != 0 && galay_coro_destroy(&client_task).code != C_IOResultOk &&
        result == 0) {
        result = 17;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 18;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 19;
    }
    return result;
}
