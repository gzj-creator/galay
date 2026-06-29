#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-redis-c/redis.h>

#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

typedef struct RedisAuthPipelineState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult server_recv_result;
    C_IOResult server_send_result;
    C_IOResult server_close_result;
    C_IOResult client_connect_result;
    C_IOResult client_auth_result;
    C_IOResult client_select_result;
    C_IOResult client_pipeline_result;
    C_IOResult client_close_result;
    galay_redis_reply_t** replies;
    size_t reply_count;
    char request[128];
} RedisAuthPipelineState;

static int create_listener(galay_kernel_tcp_socket_t* listener, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_kernel_tcp_socket_create(listener, C_IPTypeIPV4) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_bind(listener, &bind_host) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_listen(listener, 16) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_local_endpoint(listener, local) == C_TcpSocketSuccess &&
        local->port != 0
        ? 0
        : 1;
}

static int recv_exact(galay_kernel_tcp_socket_t* socket, char* buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        C_IOResult result =
            galay_kernel_tcp_socket_recv(socket, buffer + received, length - received, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        received += result.bytes;
    }
    return 0;
}

static int expect_request(galay_kernel_tcp_socket_t* socket,
                          char* buffer,
                          const char* expected,
                          size_t expected_len)
{
    return recv_exact(socket, buffer, expected_len) == 0 &&
        memcmp(buffer, expected, expected_len) == 0
        ? 0
        : 1;
}

static void redis_server_entry(void* arg)
{
    static const char auth_request[] =
        "*3\r\n$4\r\nAUTH\r\n$7\r\ndefault\r\n$6\r\nsecret\r\n";
    static const char select_request[] = "*2\r\n$6\r\nSELECT\r\n$1\r\n2\r\n";
    static const char ping_request[] = "*1\r\n$4\r\nPING\r\n";
    static const char echo_request[] = "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";
    static const char ok_response[] = "+OK\r\n";
    static const char pipeline_response[] = "+PONG\r\n+hello\r\n";
    RedisAuthPipelineState* state = (RedisAuthPipelineState*)arg;

    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }

    if (expect_request(&state->accepted, state->request, auth_request, sizeof(auth_request) - 1) !=
        0) {
        state->server_recv_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    state->server_send_result =
        galay_kernel_tcp_socket_send(&state->accepted, ok_response, sizeof(ok_response) - 1, 1000);
    if (state->server_send_result.code != C_IOResultOk) {
        return;
    }

    if (expect_request(
            &state->accepted, state->request, select_request, sizeof(select_request) - 1) != 0) {
        state->server_recv_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    state->server_send_result =
        galay_kernel_tcp_socket_send(&state->accepted, ok_response, sizeof(ok_response) - 1, 1000);
    if (state->server_send_result.code != C_IOResultOk) {
        return;
    }

    if (expect_request(&state->accepted, state->request, ping_request, sizeof(ping_request) - 1) !=
            0 ||
        expect_request(&state->accepted, state->request, echo_request, sizeof(echo_request) - 1) !=
            0) {
        state->server_recv_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    state->server_recv_result = (C_IOResult){C_IOResultOk, 0, 0, 0, NULL};
    state->server_send_result = galay_kernel_tcp_socket_send(
        &state->accepted, pipeline_response, sizeof(pipeline_response) - 1, 1000);
    if (state->server_send_result.code != C_IOResultOk) {
        return;
    }
    state->server_close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void redis_client_entry(void* arg)
{
    RedisAuthPipelineState* state = (RedisAuthPipelineState*)arg;
    galay_redis_client_config_t config = {
        .host = state->peer.address,
        .port = state->peer.port,
        .username = NULL,
        .password = NULL,
        .db_index = 0,
        .resp_version = 2,
        .connect_timeout_ms = 1000,
    };
    galay_redis_client_t* client = NULL;
    galay_redis_pipeline_t* pipeline = NULL;
    const char* echo_args[] = {"hello"};

    if (galay_redis_client_create(&config, &client) != GALAY_OK || client == NULL) {
        state->client_connect_result.code = C_IOResultError;
        return;
    }
    state->client_connect_result = galay_redis_client_connect(client, 1000);
    if (state->client_connect_result.code != C_IOResultOk) {
        galay_redis_client_destroy(client);
        return;
    }

    state->client_auth_result =
        galay_redis_client_auth(client, "default", "secret", 1000);
    state->client_select_result = galay_redis_client_select(client, 2, 1000);
    if (state->client_auth_result.code == C_IOResultOk &&
        state->client_select_result.code == C_IOResultOk &&
        galay_redis_pipeline_create(&pipeline) == GALAY_OK &&
        galay_redis_pipeline_add_command(pipeline, "PING", NULL, NULL, 0) == GALAY_OK &&
        galay_redis_pipeline_add_command(pipeline, "ECHO", echo_args, NULL, 1) == GALAY_OK) {
        state->client_pipeline_result = galay_redis_client_pipeline_async(client,
                                                                          pipeline,
                                                                          1000,
                                                                          &state->replies,
                                                                          &state->reply_count);
    }
    if (pipeline != NULL) {
        galay_redis_pipeline_destroy(pipeline);
    }
    state->client_close_result = galay_redis_client_close(client, 1000);
    galay_redis_client_destroy(client);
}

static int require_simple_string(galay_redis_reply_t* reply, const char* expected)
{
    const char* value = NULL;
    size_t value_len = 0;
    return reply != NULL &&
        galay_redis_reply_type(reply) == GALAY_REDIS_RESP_SIMPLE_STRING &&
        galay_redis_reply_string(reply, &value, &value_len) == GALAY_OK &&
        value_len == strlen(expected) && memcmp(value, expected, value_len) == 0
        ? 0
        : 1;
}

static int run_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    RedisAuthPipelineState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int result = 0;

    REQUIRE_TRUE(galay_kernel_runtime_create(&runtime_config, &runtime) == C_RuntimeSuccess, 1);
    REQUIRE_TRUE(galay_kernel_runtime_start(&runtime) == C_RuntimeSuccess, 2);
    REQUIRE_TRUE(create_listener(&listener, &local) == 0, 3);
    state.listener = &listener;
    state.peer = local;

    REQUIRE_TRUE(galay_coro_spawn(&runtime, redis_server_entry, &state, NULL, &server).code ==
                     C_IOResultOk,
                 4);
    REQUIRE_TRUE(galay_coro_spawn(&runtime, redis_client_entry, &state, NULL, &client).code ==
                     C_IOResultOk,
                 5);
    REQUIRE_TRUE(galay_coro_join(&server, 2000).code == C_IOResultOk, 6);
    REQUIRE_TRUE(galay_coro_join(&client, 2000).code == C_IOResultOk, 7);

    if (state.accept_result.code != C_IOResultOk ||
        state.server_recv_result.code != C_IOResultOk ||
        state.server_send_result.code != C_IOResultOk ||
        state.server_close_result.code != C_IOResultOk ||
        state.client_connect_result.code != C_IOResultOk ||
        state.client_auth_result.code != C_IOResultOk ||
        state.client_select_result.code != C_IOResultOk ||
        state.client_pipeline_result.code != C_IOResultOk ||
        state.client_close_result.code != C_IOResultOk ||
        state.reply_count != 2 ||
        require_simple_string(state.replies[0], "PONG") != 0 ||
        require_simple_string(state.replies[1], "hello") != 0) {
        result = 8;
    }

    if (state.replies != NULL) {
        galay_redis_pipeline_replies_destroy(state.replies, state.reply_count);
    }
    if (server.task != NULL && galay_coro_destroy(&server).code != C_IOResultOk && result == 0) {
        result = 9;
    }
    if (client.task != NULL && galay_coro_destroy(&client).code != C_IOResultOk && result == 0) {
        result = 10;
    }
    if (state.accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&state.accepted) != C_TcpSocketSuccess &&
        result == 0) {
        result = 11;
    }
    if (galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess && result == 0) {
        result = 12;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 13;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 14;
    }
    return result;
}

int main(void)
{
    return run_loopback();
}
