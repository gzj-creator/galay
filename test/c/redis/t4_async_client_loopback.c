#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-redis-c/redis_c.h>

#include <string.h>

#define REQUIRE_TRUE(expr, code) \
    do { \
        if (!(expr)) { \
            return (code); \
        } \
    } while (0)

typedef struct RedisLoopbackState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult server_recv_result;
    C_IOResult server_send_result;
    C_IOResult server_close_result;
    C_IOResult client_connect_result;
    C_IOResult client_command_result;
    C_IOResult client_close_result;
    galay_redis_reply_t* reply;
    char request[64];
} RedisLoopbackState;

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

static void redis_server_entry(void* arg)
{
    static const char expected[] = "*1\r\n$4\r\nPING\r\n";
    static const char response[] = "+PONG\r\n";
    RedisLoopbackState* state = (RedisLoopbackState*)arg;

    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }

    state->server_recv_result =
        galay_kernel_tcp_socket_recv(&state->accepted, state->request, sizeof(expected) - 1, 1000);
    if (state->server_recv_result.code != C_IOResultOk ||
        state->server_recv_result.bytes != sizeof(expected) - 1 ||
        memcmp(state->request, expected, sizeof(expected) - 1) != 0) {
        return;
    }

    state->server_send_result =
        galay_kernel_tcp_socket_send(&state->accepted, response, sizeof(response) - 1, 1000);
    if (state->server_send_result.code != C_IOResultOk) {
        return;
    }
    state->server_close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static void redis_client_entry(void* arg)
{
    RedisLoopbackState* state = (RedisLoopbackState*)arg;
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

    if (galay_redis_client_create(&config, &client) != GALAY_OK || client == NULL) {
        state->client_connect_result.code = C_IOResultError;
        return;
    }
    state->client_connect_result = galay_redis_client_connect(client, 1000);
    if (state->client_connect_result.code == C_IOResultOk) {
        state->client_command_result =
            galay_redis_client_command_async(client, "PING", NULL, NULL, 0, 1000, &state->reply);
        state->client_close_result = galay_redis_client_close(client, 1000);
    }
    galay_redis_client_destroy(client);
}

static int run_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    RedisLoopbackState state = {0};
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

    const char* value = NULL;
    size_t value_len = 0;
    if (state.accept_result.code != C_IOResultOk ||
        state.server_recv_result.code != C_IOResultOk ||
        state.server_send_result.code != C_IOResultOk ||
        state.server_close_result.code != C_IOResultOk ||
        state.client_connect_result.code != C_IOResultOk ||
        state.client_command_result.code != C_IOResultOk ||
        state.client_close_result.code != C_IOResultOk ||
        state.reply == NULL ||
        galay_redis_reply_type(state.reply) != GALAY_REDIS_RESP_SIMPLE_STRING ||
        galay_redis_reply_string(state.reply, &value, &value_len) != GALAY_OK ||
        value_len != 4 ||
        memcmp(value, "PONG", 4) != 0) {
        result = 8;
    }

    if (state.reply != NULL) {
        galay_redis_reply_destroy(state.reply);
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
