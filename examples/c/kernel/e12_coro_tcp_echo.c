#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>
#include <string.h>

typedef struct EchoExample {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    galay_kernel_tcp_socket_t client;
    char server_buffer[32];
    char client_buffer[32];
    C_IOResult server_result;
    C_IOResult client_result;
} EchoExample;

static void server_entry(void* arg)
{
    static const char response[] = "pong";
    EchoExample* example = (EchoExample*)arg;

    C_IOResult accepted = galay_kernel_tcp_socket_accept(example->listener, &example->accepted, NULL, 1000);
    if (accepted.code != C_IOResultOk) {
        example->server_result = accepted;
        return;
    }

    C_IOResult received = galay_kernel_tcp_socket_recv(&example->accepted,
                                              example->server_buffer,
                                              sizeof(example->server_buffer),
                                              1000);
    if (received.code != C_IOResultOk) {
        example->server_result = received;
        return;
    }

    C_IOResult sent = galay_kernel_tcp_socket_send(&example->accepted,
                                          response,
                                          sizeof(response) - 1,
                                          1000);
    example->server_result = sent;
    (void)galay_kernel_tcp_socket_close(&example->accepted, 1000);
}

static void client_entry(void* arg)
{
    static const char request[] = "ping";
    EchoExample* example = (EchoExample*)arg;

    if (galay_kernel_tcp_socket_create(&example->client, C_IPTypeIPV4) != C_TcpSocketSuccess) {
        example->client_result = (C_IOResult){C_IOResultError, 0, 0, 0, 0};
        return;
    }

    C_IOResult connected = galay_kernel_tcp_socket_connect(&example->client, &example->peer, 1000);
    if (connected.code != C_IOResultOk) {
        example->client_result = connected;
        return;
    }

    C_IOResult sent = galay_kernel_tcp_socket_send(&example->client,
                                          request,
                                          sizeof(request) - 1,
                                          1000);
    if (sent.code != C_IOResultOk) {
        example->client_result = sent;
        return;
    }

    C_IOResult received = galay_kernel_tcp_socket_recv(&example->client,
                                              example->client_buffer,
                                              sizeof(example->client_buffer),
                                              1000);
    example->client_result = received;
    (void)galay_kernel_tcp_socket_close(&example->client, 1000);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host local = {0};
    EchoExample example;
    int exit_code = 0;
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    memset(&example, 0, sizeof(example));
    example.listener = &listener;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_tcp_socket_create(&listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(&listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&listener, 16) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_local_endpoint(&listener, &local) != C_TcpSocketSuccess) {
        exit_code = 1;
        goto cleanup;
    }
    example.peer = local;

    if (galay_coro_spawn(&runtime, server_entry, &example, 0, &server).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &example, 0, &client).code != C_IOResultOk ||
        galay_coro_join(&server, 2000).code != C_IOResultOk ||
        galay_coro_join(&client, 2000).code != C_IOResultOk) {
        exit_code = 2;
        goto cleanup;
    }

    const int ok =
        example.server_result.code == C_IOResultOk &&
        example.client_result.code == C_IOResultOk &&
        memcmp(example.server_buffer, "ping", 4) == 0 &&
        memcmp(example.client_buffer, "pong", 4) == 0;

    printf("coro_tcp_echo request=%.*s response=%.*s port=%u\n",
           4,
           example.server_buffer,
           4,
           example.client_buffer,
           local.port);

    exit_code = ok ? 0 : 3;

cleanup:
    if (server.task != 0) {
        (void)galay_coro_destroy(&server);
    }
    if (client.task != 0) {
        (void)galay_coro_destroy(&client);
    }
    if (example.accepted.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&example.accepted);
    }
    if (example.client.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&example.client);
    }
    (void)galay_kernel_tcp_socket_destroy(&listener);
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}
