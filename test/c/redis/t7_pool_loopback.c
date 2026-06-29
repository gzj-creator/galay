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

typedef struct RedisPoolLoopbackState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult server_result;
    C_IOResult client_first_acquire;
    C_IOResult client_pipeline;
    C_IOResult client_second_acquire;
    C_IOResult client_ping;
    galay_status_t first_release;
    galay_status_t second_release;
    galay_redis_reply_t** replies;
    size_t reply_count;
    galay_redis_reply_t* ping_reply;
    char request[128];
} RedisPoolLoopbackState;

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

static int send_exact(galay_kernel_tcp_socket_t* socket, const char* buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        C_IOResult result =
            galay_kernel_tcp_socket_send(socket, buffer + sent, length - sent, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        sent += result.bytes;
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
    static const char ping_request[] = "*1\r\n$4\r\nPING\r\n";
    static const char echo_request[] = "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";
    static const char pipeline_response[] = "+PONG\r\n$5\r\nhello\r\n";
    static const char integer_response[] = ":42\r\n";
    RedisPoolLoopbackState* state = (RedisPoolLoopbackState*)arg;

    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    if (expect_request(&state->accepted, state->request, ping_request, sizeof(ping_request) - 1) !=
            0 ||
        expect_request(&state->accepted, state->request, echo_request, sizeof(echo_request) - 1) !=
            0 ||
        send_exact(&state->accepted, pipeline_response, sizeof(pipeline_response) - 1) != 0 ||
        expect_request(&state->accepted, state->request, ping_request, sizeof(ping_request) - 1) !=
            0 ||
        send_exact(&state->accepted, integer_response, sizeof(integer_response) - 1) != 0) {
        state->server_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    state->server_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static int require_string_reply(galay_redis_reply_t* reply, const char* expected)
{
    const char* value = NULL;
    size_t value_len = 0;
    return reply != NULL &&
        galay_redis_reply_string(reply, &value, &value_len) == GALAY_OK &&
        value_len == strlen(expected) &&
        memcmp(value, expected, value_len) == 0
        ? 0
        : 1;
}

static void redis_client_entry(void* arg)
{
    RedisPoolLoopbackState* state = (RedisPoolLoopbackState*)arg;
    galay_redis_pool_t* pool = NULL;
    galay_redis_pool_lease_t* lease = NULL;
    galay_redis_pool_lease_t* second_lease = NULL;
    galay_redis_pipeline_t* pipeline = NULL;
    const char* echo_args[] = {"hello"};
    galay_redis_pool_config_t pool_config = {
        .client = {
            .host = state->peer.address,
            .port = state->peer.port,
            .username = NULL,
            .password = NULL,
            .db_index = 0,
            .resp_version = 2,
            .connect_timeout_ms = 1000,
        },
        .min_connections = 0,
        .max_connections = 1,
        .initial_connections = 0,
    };

    if (galay_redis_pool_create(&pool_config, &pool) != GALAY_OK ||
        galay_redis_pipeline_create(&pipeline) != GALAY_OK ||
        galay_redis_pipeline_add_command(pipeline, "PING", NULL, NULL, 0) != GALAY_OK ||
        galay_redis_pipeline_add_command(pipeline, "ECHO", echo_args, NULL, 1) != GALAY_OK) {
        goto cleanup;
    }

    state->client_first_acquire = galay_redis_pool_acquire(pool, 1000, &lease);
    if (state->client_first_acquire.code == C_IOResultOk) {
        state->client_pipeline =
            galay_redis_client_pipeline_async(galay_redis_pool_lease_client(lease),
                                              pipeline,
                                              1000,
                                              &state->replies,
                                              &state->reply_count);
        state->first_release = galay_redis_pool_release(pool, lease);
        lease = NULL;
    }

    state->client_second_acquire = galay_redis_pool_acquire(pool, 1000, &second_lease);
    if (state->client_second_acquire.code == C_IOResultOk) {
        state->client_ping =
            galay_redis_client_command_async(galay_redis_pool_lease_client(second_lease),
                                             "PING",
                                             NULL,
                                             NULL,
                                             0,
                                             1000,
                                             &state->ping_reply);
        state->second_release = galay_redis_pool_release(pool, second_lease);
        second_lease = NULL;
    }

cleanup:
    if (lease != NULL) {
        state->first_release = galay_redis_pool_release(pool, lease);
    }
    if (second_lease != NULL) {
        state->second_release = galay_redis_pool_release(pool, second_lease);
    }
    if (pipeline != NULL) {
        galay_redis_pipeline_destroy(pipeline);
    }
    if (pool != NULL) {
        galay_redis_pool_destroy(pool);
    }
}

static int run_loopback(void)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    RedisPoolLoopbackState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int64_t integer = 0;
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
        state.server_result.code != C_IOResultOk ||
        state.client_first_acquire.code != C_IOResultOk ||
        state.client_pipeline.code != C_IOResultOk ||
        state.first_release != GALAY_OK ||
        state.client_second_acquire.code != C_IOResultOk ||
        state.client_ping.code != C_IOResultOk ||
        state.second_release != GALAY_OK ||
        state.reply_count != 2 ||
        require_string_reply(state.replies[0], "PONG") != 0 ||
        require_string_reply(state.replies[1], "hello") != 0 ||
        galay_redis_reply_integer(state.ping_reply, &integer) != GALAY_OK ||
        integer != 42) {
        result = 8;
    }

    if (state.replies != NULL) {
        galay_redis_pipeline_replies_destroy(state.replies, state.reply_count);
    }
    if (state.ping_reply != NULL) {
        galay_redis_reply_free(state.ping_reply);
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
