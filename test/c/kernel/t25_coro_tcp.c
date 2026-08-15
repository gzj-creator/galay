#include <galay/c/galay-kernel-c/async-c/tcp_socket.h>
#include <galay/c/galay-common-c/common/galay_c_iovec.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include "socket_test_support.h"

#include <string.h>
#include <sys/socket.h>

typedef struct TcpEchoState {
    galay_c_tcp_socket_t* listener;
    galay_c_tcp_socket_t accepted;
    galay_c_tcp_socket_t client;
    C_Host endpoint;
    C_IOResult server_result;
    C_IOResult client_result;
    char server_buffer[32];
    char client_buffer[32];
} TcpEchoState;

static C_IOResult send_all(galay_c_tcp_socket_t* socket,
                           const char* buffer,
                           size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        const C_IOResult result =
            galay_c_tcp_socket_send(socket, buffer + sent, length - sent, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return result;
        }
        sent += result.bytes;
    }
    return (C_IOResult){C_IOResultOk, 0, sent, 0, NULL};
}

static C_IOResult recv_all(galay_c_tcp_socket_t* socket,
                           char* buffer,
                           size_t length)
{
    size_t received = 0;
    while (received < length) {
        const C_IOResult result =
            galay_c_tcp_socket_recv(socket, buffer + received, length - received, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return result;
        }
        received += result.bytes;
    }
    return (C_IOResult){C_IOResultOk, 0, received, 0, NULL};
}

static void server_entry(void* arg)
{
    TcpEchoState* const state = arg;
    state->server_result =
        galay_c_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->server_result.code != C_IOResultOk) {
        return;
    }
    state->server_result =
        recv_all(&state->accepted, state->server_buffer, strlen("native-ping"));
    if (state->server_result.code == C_IOResultOk) {
        state->server_result =
            send_all(&state->accepted, "native-pong", strlen("native-pong"));
    }
    const C_IOResult closed = galay_c_tcp_socket_close(&state->accepted);
    if (state->server_result.code == C_IOResultOk && closed.code != C_IOResultOk) {
        state->server_result = closed;
    }
}

static void client_entry(void* arg)
{
    TcpEchoState* const state = arg;
    state->client_result =
        galay_c_tcp_socket_connect(&state->client, &state->endpoint, 1000);
    if (state->client_result.code != C_IOResultOk) {
        return;
    }
    state->client_result = send_all(&state->client, "native-ping", strlen("native-ping"));
    if (state->client_result.code == C_IOResultOk) {
        state->client_result =
            recv_all(&state->client, state->client_buffer, strlen("native-pong"));
    }
    const C_IOResult closed = galay_c_tcp_socket_close(&state->client);
    if (state->client_result.code == C_IOResultOk && closed.code != C_IOResultOk) {
        state->client_result = closed;
    }
}

int main(void)
{
    galay_c_tcp_socket_t invalid = {.fd = -1};
    C_Host invalid_host = {C_IPTypeIPV4, "not-an-ip", 0};
    char byte = 0;
    galay_iovec_t invalid_iovec = {&byte, 1};
    if (galay_c_tcp_socket_create(NULL, C_IPTypeIPV4).code != C_IOResultInvalid ||
        galay_c_tcp_socket_create(&invalid, (C_IPType)99).code != C_IOResultInvalid ||
        galay_c_tcp_socket_accept(NULL, &invalid, NULL, 0).code != C_IOResultInvalid ||
        galay_c_tcp_socket_connect(&invalid, &invalid_host, 0).code != C_IOResultInvalid ||
        galay_c_tcp_socket_recv(&invalid, &byte, 1, 0).code != C_IOResultInvalid ||
        galay_c_tcp_socket_send(&invalid, &byte, 1, 0).code != C_IOResultInvalid ||
        galay_c_tcp_socket_readv(&invalid, &invalid_iovec, 1, 0).code != C_IOResultInvalid ||
        galay_c_tcp_socket_writev(&invalid, &invalid_iovec, 1, 0).code != C_IOResultInvalid ||
        galay_c_tcp_socket_sendfile(&invalid, -1, 0, 1, 0).code != C_IOResultInvalid ||
        galay_c_tcp_socket_close(NULL).code != C_IOResultInvalid) {
        return 1;
    }

    const galay_test_socket_capability_t socket_capability =
        galay_test_socket_capability(SOCK_STREAM);
    if (socket_capability == GALAY_TEST_SOCKET_PERMISSION_DENIED) {
        return 125;
    }
    if (socket_capability == GALAY_TEST_SOCKET_PROBE_FAILED) {
        return 126;
    }

    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_tcp_socket_t listener = {.fd = -1};
    galay_c_coro_task_t server_task = {0};
    galay_c_coro_task_t client_task = {0};
    TcpEchoState state = {
        .listener = &listener,
        .accepted = {.fd = -1},
        .client = {.fd = -1},
        .server_result = {C_IOResultInvalid, 0, 0, 0, NULL},
        .client_result = {C_IOResultInvalid, 0, 0, 0, NULL},
    };
    const C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    int result = 0;

    if (galay_c_tcp_socket_create(&listener, C_IPTypeIPV4).code != C_IOResultOk ||
        galay_c_tcp_socket_bind(&listener, &bind_host).code != C_IOResultOk ||
        galay_c_tcp_socket_listen(&listener, 16).code != C_IOResultOk ||
        galay_c_tcp_socket_local_endpoint(&listener, &state.endpoint).code != C_IOResultOk ||
        galay_c_tcp_socket_create(&state.client, C_IPTypeIPV4).code != C_IOResultOk) {
        result = 2;
        goto cleanup;
    }
    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_c_runtime_start(&runtime) != C_RuntimeSuccess) {
        result = 3;
        goto cleanup;
    }
    if (galay_c_coro_spawn(&runtime, server_entry, &state, NULL, &server_task).code !=
            C_IOResultOk ||
        galay_c_coro_spawn(&runtime, client_entry, &state, NULL, &client_task).code !=
            C_IOResultOk ||
        galay_c_coro_join(&server_task, 3000).code != C_IOResultOk ||
        galay_c_coro_join(&client_task, 3000).code != C_IOResultOk) {
        result = 4;
    } else if (state.server_result.code != C_IOResultOk ||
               state.client_result.code != C_IOResultOk ||
               memcmp(state.server_buffer, "native-ping", strlen("native-ping")) != 0 ||
               memcmp(state.client_buffer, "native-pong", strlen("native-pong")) != 0) {
        result = 5;
    }

cleanup:
    if (server_task.task != NULL && galay_c_coro_destroy(&server_task).code != C_IOResultOk &&
        result == 0) {
        result = 6;
    }
    if (client_task.task != NULL && galay_c_coro_destroy(&client_task).code != C_IOResultOk &&
        result == 0) {
        result = 7;
    }
    if (state.accepted.fd >= 0 && galay_c_tcp_socket_close(&state.accepted).code !=
            C_IOResultOk &&
        result == 0) {
        result = 8;
    }
    if (state.client.fd >= 0 && galay_c_tcp_socket_close(&state.client).code != C_IOResultOk &&
        result == 0) {
        result = 9;
    }
    if (listener.fd >= 0 && galay_c_tcp_socket_close(&listener).code != C_IOResultOk &&
        result == 0) {
        result = 10;
    }
    if (runtime.runtime != NULL) {
        if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 11;
        }
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
            result = 12;
        }
    }
    return result;
}
