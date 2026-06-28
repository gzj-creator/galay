#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct ServerState {
    galay_kernel_tcp_socket_t* listener;
    C_IOResult accept_result;
    C_IOResult recv_result;
    C_IOResult send_result;
    C_IOResult close_result;
    galay_kernel_tcp_socket_t accepted;
    char recv_buffer[16];
} ServerState;

static void server_entry(void* ctx)
{
    static const char response[] = "pong";
    ServerState* state = (ServerState*)ctx;
    state->accept_result =
        galay_kernel_tcp_socket_accept(state->listener, &state->accepted, 0, 2000);
    if (state->accept_result.code != C_IOResultOk) {
        return;
    }
    state->recv_result =
        galay_kernel_tcp_socket_recv(&state->accepted, state->recv_buffer, sizeof(state->recv_buffer), 2000);
    if (state->recv_result.code != C_IOResultOk) {
        return;
    }
    state->send_result =
        galay_kernel_tcp_socket_send(&state->accepted, response, sizeof(response) - 1, 2000);
    state->close_result = galay_kernel_tcp_socket_close(&state->accepted, 2000);
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

static int wait_and_read(int fd, char* buffer, size_t length)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 2000) <= 0 || (pfd.revents & POLLIN) == 0) {
        return -1;
    }
    return (int)recv(fd, buffer, length, 0);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    galay_coro_task_t server_task = {0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host local = {0};
    int client_fd = -1;
    int exit_code = 0;

    const char request[] = "ping";
    char client_buffer[16] = {0};
    ServerState state = {0};
    state.listener = &listener;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_tcp_socket_create(&listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(&listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&listener, 16) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_local_endpoint(&listener, &local) != C_TcpSocketSuccess ||
        galay_coro_spawn(&runtime, server_entry, &state, 0, &server_task).code != C_IOResultOk) {
        exit_code = 1;
        goto cleanup;
    }

    client_fd = connect_posix_client(local.port);
    if (client_fd < 0 ||
        send(client_fd, request, sizeof(request) - 1, 0) != (ssize_t)(sizeof(request) - 1) ||
        wait_and_read(client_fd, client_buffer, sizeof(client_buffer)) != 4 ||
        memcmp(client_buffer, "pong", 4) != 0 ||
        galay_coro_join(&server_task, 3000).code != C_IOResultOk) {
        exit_code = 2;
        goto cleanup;
    }

    if (state.accept_result.code != C_IOResultOk ||
        state.recv_result.code != C_IOResultOk ||
        state.recv_result.bytes != sizeof(request) - 1 ||
        memcmp(state.recv_buffer, request, sizeof(request) - 1) != 0 ||
        state.send_result.code != C_IOResultOk ||
        state.send_result.bytes != 4 ||
        state.close_result.code != C_IOResultOk) {
        exit_code = 3;
        goto cleanup;
    }

    if (printf("tcp_socket_echo request=%s response=%s port=%u\n",
               request,
               client_buffer,
               local.port) < 0) {
        exit_code = 10;
        goto cleanup;
    }

cleanup:
    if (client_fd >= 0) {
        if (close(client_fd) != 0 && exit_code == 0) {
            exit_code = 4;
        }
    }
    if (server_task.task != 0) {
        if (galay_coro_destroy(&server_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 5;
        }
    }
    if (state.accepted.socket != 0) {
        if (galay_kernel_tcp_socket_destroy(&state.accepted) != C_TcpSocketSuccess && exit_code == 0) {
            exit_code = 6;
        }
    }
    if (listener.socket != 0) {
        if (galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess && exit_code == 0) {
            exit_code = 7;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 8;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 9;
        }
    }
    return exit_code;
}
