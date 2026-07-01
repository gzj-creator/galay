#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-redis-c/redis_c.h>

#include <stdio.h>
#include <string.h>

typedef struct RedisAsyncPingExample {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult server_result;
    C_IOResult client_result;
    char request[64];
    char response[16];
} RedisAsyncPingExample;

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

static void server_entry(void* arg)
{
    static const char expected[] = "*1\r\n$4\r\nPING\r\n";
    static const char response[] = "+PONG\r\n";
    RedisAsyncPingExample* example = (RedisAsyncPingExample*)arg;

    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(example->listener, &example->accepted, NULL, 1000);
    if (accepted.code != C_IOResultOk) {
        example->server_result = accepted;
        return;
    }
    C_IOResult received = galay_kernel_tcp_socket_recv(
        &example->accepted, example->request, sizeof(expected) - 1, 1000);
    if (received.code != C_IOResultOk ||
        received.bytes != sizeof(expected) - 1 ||
        memcmp(example->request, expected, sizeof(expected) - 1) != 0) {
        example->server_result = received.code == C_IOResultOk
            ? (C_IOResult){C_IOResultError, 0, 0, 0, NULL}
            : received;
        return;
    }
    C_IOResult sent =
        galay_kernel_tcp_socket_send(&example->accepted, response, sizeof(response) - 1, 1000);
    example->server_result = sent;
    C_IOResult closed = galay_kernel_tcp_socket_close(&example->accepted, 1000);
    if (example->server_result.code == C_IOResultOk && closed.code != C_IOResultOk) {
        example->server_result = closed;
    }
}

static void client_entry(void* arg)
{
    RedisAsyncPingExample* example = (RedisAsyncPingExample*)arg;
    galay_redis_client_config_t config = {
        .host = example->peer.address,
        .port = example->peer.port,
        .username = NULL,
        .password = NULL,
        .db_index = 0,
        .resp_version = 2,
        .connect_timeout_ms = 1000,
    };
    galay_redis_client_t* client = NULL;
    galay_redis_reply_t* reply = NULL;
    const char* value = NULL;
    size_t value_len = 0;

    if (galay_redis_client_create(&config, &client) != GALAY_OK || client == NULL) {
        example->client_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    C_IOResult connected = galay_redis_client_connect(client, 1000);
    if (connected.code != C_IOResultOk) {
        example->client_result = connected;
        galay_redis_client_destroy(client);
        return;
    }
    example->client_result =
        galay_redis_client_command_async(client, "PING", NULL, NULL, 0, 1000, &reply);
    if (example->client_result.code == C_IOResultOk &&
        galay_redis_reply_string(reply, &value, &value_len) == GALAY_OK &&
        value_len < sizeof(example->response)) {
        memcpy(example->response, value, value_len);
        example->response[value_len] = '\0';
    }
    if (reply != NULL) {
        galay_redis_reply_destroy(reply);
    }
    C_IOResult closed = galay_redis_client_close(client, 1000);
    if (example->client_result.code == C_IOResultOk && closed.code != C_IOResultOk) {
        example->client_result = closed;
    }
    galay_redis_client_destroy(client);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    RedisAsyncPingExample example = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, &local) != 0) {
        exit_code = 1;
        goto cleanup;
    }
    example.listener = &listener;
    example.peer = local;

    if (galay_coro_spawn(&runtime, server_entry, &example, NULL, &server).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &example, NULL, &client).code != C_IOResultOk ||
        galay_coro_join(&server, 2000).code != C_IOResultOk ||
        galay_coro_join(&client, 2000).code != C_IOResultOk) {
        exit_code = 2;
        goto cleanup;
    }
    if (example.server_result.code != C_IOResultOk ||
        example.client_result.code != C_IOResultOk ||
        strcmp(example.response, "PONG") != 0) {
        exit_code = 3;
        goto cleanup;
    }
    if (printf("c_redis_async_ping response=%s port=%u\n",
               example.response,
               local.port) < 0) {
        exit_code = 4;
    }

cleanup:
    if (server.task != NULL && galay_coro_destroy(&server).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 5;
    }
    if (client.task != NULL && galay_coro_destroy(&client).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 6;
    }
    if (example.accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&example.accepted) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 7;
    }
    if (listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 8;
    }
    if (runtime.runtime != NULL) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 9;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 10;
        }
    }
    return exit_code;
}
