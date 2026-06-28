#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum {
    TCP_SOCKET_ITERATIONS = 64
};

typedef struct AcceptCloseState {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    C_IOResult accept_result;
    C_IOResult close_result;
} AcceptCloseState;

static void accept_close_entry(void* arg)
{
    AcceptCloseState* state = (AcceptCloseState*)arg;
    state->accept_result = galay_kernel_tcp_socket_accept(state->listener, &state->accepted, NULL, 1000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    state->close_result = galay_kernel_tcp_socket_close(&state->accepted, 1000);
}

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
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
            return -1;
        }
        return -1;
    }
    return fd;
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
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_tcp_socket_create(&listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(&listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&listener, 64) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_local_endpoint(&listener, &local) != C_TcpSocketSuccess ||
        local.port == 0) {
        exit_code = 1;
        goto cleanup;
    }

    const int64_t start = now_us();
    for (int i = 0; i < TCP_SOCKET_ITERATIONS; ++i) {
        AcceptCloseState state = {
            .listener = &listener,
            .accepted = {0},
            .accept_result = {C_IOResultInvalid, 0, 0, 0, NULL},
            .close_result = {C_IOResultInvalid, 0, 0, 0, NULL},
        };
        galay_coro_task_t task = {0};
        C_IOResult spawn_result = galay_coro_spawn(&runtime, accept_close_entry, &state, NULL, &task);
        if (spawn_result.code != C_IOResultOk) {
            exit_code = 2;
            goto cleanup;
        }

        int client_fd = connect_posix_client(local.port);
        if (client_fd < 0) {
            if (galay_coro_join(&task, 0).code == C_IOResultOk) {
                if (galay_coro_destroy(&task).code != C_IOResultOk) {
                    exit_code = 3;
                    goto cleanup;
                }
            }
            exit_code = 4;
            goto cleanup;
        }

        C_IOResult join_result = galay_coro_join(&task, 2000);
        C_IOResult destroy_result = galay_coro_destroy(&task);
        if (close(client_fd) != 0) {
            exit_code = 5;
            goto cleanup;
        }
        if (join_result.code != C_IOResultOk ||
            destroy_result.code != C_IOResultOk ||
            state.accept_result.code != C_IOResultOk ||
            state.close_result.code != C_IOResultOk) {
            exit_code = 6;
            if (state.accepted.socket != NULL &&
                galay_kernel_tcp_socket_destroy(&state.accepted) != C_TcpSocketSuccess) {
                exit_code = 7;
            }
            goto cleanup;
        }
        if (state.accepted.socket != NULL &&
            galay_kernel_tcp_socket_destroy(&state.accepted) != C_TcpSocketSuccess) {
            exit_code = 8;
            goto cleanup;
        }
    }

    const int64_t elapsed = now_us() - start;
    const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
    const double ops_per_sec = seconds > 0.0 ? (double)TCP_SOCKET_ITERATIONS / seconds : 0.0;
    if (printf("tcp_socket_lifecycle iterations=%d elapsed_ms=%.3f ops_per_sec=%.2f\n",
               TCP_SOCKET_ITERATIONS,
               (double)elapsed / 1000.0,
               ops_per_sec) < 0) {
        exit_code = 9;
    }

cleanup:
    if (listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 10;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 11;
    }
    if (runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 12;
    }
    return exit_code;
}
