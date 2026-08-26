#include <galay/c/galay-kernel-c/async-c/tcp_socket.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task.h>

#include "socket_test_support.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct TcpTimeoutState {
    galay_c_tcp_socket_t* listener;
    galay_c_tcp_socket_t accepted;
    C_IOResult accept_timeout_result;
    C_IOResult accept_result;
    C_IOResult recv_timeout_result;
    C_IOResult close_result;
    char buffer[16];
} TcpTimeoutState;

static int expect_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

static int connect_posix_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
        if (close(fd) != 0) {
            return -2;
        }
        return -1;
    }
    return fd;
}

static int create_listener(galay_c_tcp_socket_t* listener, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_c_tcp_socket_create(listener, C_IPTypeIPV4).code == C_IOResultOk &&
        galay_c_tcp_socket_bind(listener, &bind_host).code == C_IOResultOk &&
        galay_c_tcp_socket_listen(listener, 16).code == C_IOResultOk &&
        galay_c_tcp_socket_local_endpoint(listener, local).code == C_IOResultOk &&
        local->port != 0
        ? 0
        : 1;
}

static void accept_timeout_entry(void* arg)
{
    TcpTimeoutState* state = (TcpTimeoutState*)arg;
    state->accept_timeout_result =
        galay_c_tcp_socket_accept(state->listener, &state->accepted, NULL, 5);
}

static void recv_timeout_entry(void* arg)
{
    TcpTimeoutState* state = (TcpTimeoutState*)arg;
    state->accept_result =
        galay_c_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    state->recv_timeout_result =
        galay_c_tcp_socket_recv(&state->accepted,
                                     state->buffer,
                                     sizeof(state->buffer),
                                     5);
    state->close_result = galay_c_tcp_socket_close(&state->accepted);
}

static int test_accept_timeout(galay_c_runtime_t* runtime)
{
    galay_c_tcp_socket_t listener = {.fd = -1};
    C_Host local = {0};
    TcpTimeoutState state = {0};
    state.listener = &listener;
    state.accepted.fd = -1;

    if (create_listener(&listener, &local) != 0) {
        return 101;
    }
    (void)local;

    galay_c_coro_task_t task = {0};
    int result = 0;
    if (expect_code(galay_c_coro_spawn(runtime, accept_timeout_entry, &state, 0, &task),
                    C_IOResultOk) ||
        expect_code(galay_c_coro_join(&task, 2000), C_IOResultOk)) {
        result = 102;
    } else if (state.accept_timeout_result.code != C_IOResultTimeout ||
               state.accepted.fd != -1) {
        result = 103;
    }

    if (galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 104;
    }
    if (state.accepted.fd >= 0) {
        if (galay_c_tcp_socket_close(&state.accepted).code != C_IOResultOk &&
            result == 0) {
            result = 105;
        }
    }
    if (galay_c_tcp_socket_close(&listener).code != C_IOResultOk && result == 0) {
        result = 106;
    }
    return result;
}

static int test_recv_timeout(galay_c_runtime_t* runtime)
{
    galay_c_tcp_socket_t listener = {.fd = -1};
    C_Host local = {0};
    int client_fd = -1;
    TcpTimeoutState state = {0};
    state.listener = &listener;
    state.accepted.fd = -1;

    if (create_listener(&listener, &local) != 0) {
        return 201;
    }

    galay_c_coro_task_t task = {0};
    int result = 0;
    if (expect_code(galay_c_coro_spawn(runtime, recv_timeout_entry, &state, 0, &task),
                    C_IOResultOk)) {
        result = 202;
        goto cleanup;
    }
    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        result = 203;
        goto cleanup;
    }
    if (expect_code(galay_c_coro_join(&task, 2000), C_IOResultOk)) {
        result = 204;
        goto cleanup;
    }
    if (state.accept_result.code != C_IOResultOk ||
        state.recv_timeout_result.code != C_IOResultTimeout ||
        state.recv_timeout_result.bytes != 0 ||
        state.close_result.code != C_IOResultOk) {
        result = 205;
    }

cleanup:
    if (client_fd >= 0) {
        if (close(client_fd) != 0 && result == 0) {
            result = 209;
        }
    }
    if (task.task != 0 && galay_c_coro_destroy(&task).code != C_IOResultOk && result == 0) {
        result = 206;
    }
    if (state.accepted.fd >= 0) {
        if (galay_c_tcp_socket_close(&state.accepted).code != C_IOResultOk &&
            result == 0) {
            result = 207;
        }
    }
    if (galay_c_tcp_socket_close(&listener).code != C_IOResultOk && result == 0) {
        result = 208;
    }
    return result;
}

int main(void)
{
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
    config.parallel_scheduler_count = 0;

    galay_c_runtime_t runtime = {0};
    int result = 0;

    if (galay_c_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_c_runtime_start(&runtime) != C_RuntimeSuccess) {
        if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess) {
            return 4;
        }
        return 2;
    }

    result = test_accept_timeout(&runtime);
    if (result == 0) {
        result = test_recv_timeout(&runtime);
    }

    if (galay_c_runtime_stop(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 5;
    }
    if (galay_c_runtime_destroy(&runtime) != C_RuntimeSuccess && result == 0) {
        result = 3;
    }
    return result;
}
