#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-redis-c/redis.h>

#include <stdio.h>
#include <string.h>

typedef struct ExampleState {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    int server_ok;
    int client_ok;
    char request[128];
} ExampleState;

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

static void server_entry(void* arg)
{
    static const char ping_request[] = "*1\r\n$4\r\nPING\r\n";
    static const char echo_request[] = "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";
    static const char response[] = "+PONG\r\n$5\r\nhello\r\n";
    ExampleState* state = (ExampleState*)arg;

    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (accepted.code != C_IOResultOk) {
        return;
    }
    if (expect_request(&state->accepted, state->request, ping_request, sizeof(ping_request) - 1) !=
            0 ||
        expect_request(&state->accepted, state->request, echo_request, sizeof(echo_request) - 1) !=
            0 ||
        send_exact(&state->accepted, response, sizeof(response) - 1) != 0) {
        return;
    }
    C_IOResult closed = galay_kernel_tcp_socket_close(&state->accepted, 1000);
    state->server_ok = closed.code == C_IOResultOk ? 1 : 0;
}

static void client_entry(void* arg)
{
    ExampleState* state = (ExampleState*)arg;
    galay_redis_pool_t* pool = NULL;
    galay_redis_pool_lease_t* lease = NULL;
    galay_redis_pipeline_t* pipeline = NULL;
    galay_redis_reply_t** replies = NULL;
    size_t reply_count = 0;
    const char* echo_args[] = {"hello"};
    galay_redis_pool_config_t config = {
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

    if (galay_redis_pool_create(&config, &pool) != GALAY_OK ||
        galay_redis_pipeline_create(&pipeline) != GALAY_OK ||
        galay_redis_pipeline_add_command(pipeline, "PING", NULL, NULL, 0) != GALAY_OK ||
        galay_redis_pipeline_add_command(pipeline, "ECHO", echo_args, NULL, 1) != GALAY_OK) {
        goto cleanup;
    }
    C_IOResult acquired = galay_redis_pool_acquire(pool, 1000, &lease);
    if (acquired.code != C_IOResultOk) {
        goto cleanup;
    }
    C_IOResult piped = galay_redis_client_pipeline_async(galay_redis_pool_lease_client(lease),
                                                         pipeline,
                                                         1000,
                                                         &replies,
                                                         &reply_count);
    if (piped.code == C_IOResultOk && reply_count == 2) {
        const char* pong = NULL;
        const char* echo = NULL;
        size_t pong_len = 0;
        size_t echo_len = 0;
        state->client_ok =
            galay_redis_reply_string(replies[0], &pong, &pong_len) == GALAY_OK &&
            galay_redis_reply_string(replies[1], &echo, &echo_len) == GALAY_OK &&
            pong_len == 4 &&
            echo_len == 5 &&
            memcmp(pong, "PONG", pong_len) == 0 &&
            memcmp(echo, "hello", echo_len) == 0
            ? 1
            : 0;
    }
    if (galay_redis_pool_release(pool, lease) == GALAY_OK) {
        lease = NULL;
    }

cleanup:
    if (lease != NULL) {
        galay_status_t released = galay_redis_pool_release(pool, lease);
        if (released != GALAY_OK) {
            state->client_ok = 0;
        }
    }
    if (replies != NULL) {
        galay_redis_pipeline_replies_destroy(replies, reply_count);
    }
    if (pipeline != NULL) {
        galay_redis_pipeline_destroy(pipeline);
    }
    if (pool != NULL) {
        galay_redis_pool_destroy(pool);
    }
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    ExampleState state = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, &local) != 0) {
        exit_code = 1;
        goto cleanup;
    }
    state.listener = &listener;
    state.peer = local;
    if (galay_coro_spawn(&runtime, server_entry, &state, NULL, &server).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &state, NULL, &client).code != C_IOResultOk ||
        galay_coro_join(&server, 2000).code != C_IOResultOk ||
        galay_coro_join(&client, 2000).code != C_IOResultOk ||
        state.server_ok != 1 ||
        state.client_ok != 1) {
        exit_code = 2;
        goto cleanup;
    }
    if (printf("redis pool pipeline loopback ok\n") < 0) {
        exit_code = 3;
    }

cleanup:
    if (server.task != NULL && galay_coro_destroy(&server).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 4;
    }
    if (client.task != NULL && galay_coro_destroy(&client).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 5;
    }
    if (state.accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&state.accepted) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 6;
    }
    if (listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 7;
    }
    if (runtime.runtime != NULL) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 8;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 9;
        }
    }
    return exit_code;
}
