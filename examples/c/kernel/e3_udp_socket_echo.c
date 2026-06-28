#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>
#include <string.h>

typedef struct RecvState {
    galay_kernel_udp_socket_t* socket;
    C_IOResult result;
    char* buffer;
    size_t length;
    C_Host from;
} RecvState;

typedef struct SendState {
    galay_kernel_udp_socket_t* socket;
    C_IOResult result;
    const char* buffer;
    size_t length;
    C_Host to;
} SendState;

typedef struct CloseState {
    galay_kernel_udp_socket_t* socket;
    C_IOResult result;
} CloseState;

static void recv_entry(void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    state->result =
        galay_kernel_udp_socket_recvfrom(state->socket,
                                         state->buffer,
                                         state->length,
                                         &state->from,
                                         2000);
}

static void send_entry(void* ctx)
{
    SendState* state = (SendState*)ctx;
    state->result =
        galay_kernel_udp_socket_sendto(state->socket,
                                       state->buffer,
                                       state->length,
                                       &state->to,
                                       2000);
}

static void close_entry(void* ctx)
{
    CloseState* state = (CloseState*)ctx;
    state->result = galay_kernel_udp_socket_close(state->socket, 1000);
}

static int close_socket(galay_kernel_runtime_t* runtime, galay_kernel_udp_socket_t* socket)
{
    if (socket->socket == 0) {
        return 0;
    }
    galay_coro_task_t task = {0};
    CloseState state = {socket, {0}};
    if (galay_coro_spawn(runtime, close_entry, &state, 0, &task).code != C_IOResultOk ||
        galay_coro_join(&task, 2000).code != C_IOResultOk ||
        state.result.code != C_IOResultOk) {
        if (task.task != 0) {
            if (galay_coro_destroy(&task).code != C_IOResultOk) {
                return 1;
            }
        }
        return 1;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk) {
        return 1;
    }
    return 0;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_udp_socket_t server = {0};
    galay_kernel_udp_socket_t client = {0};
    galay_coro_task_t recv_task = {0};
    galay_coro_task_t send_task = {0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host server_local = {0};
    C_Host client_local = {0};
    int exit_code = 0;

    const char request[] = "ping";
    const char response[] = "pong";
    char server_buffer[16] = {0};
    char client_buffer[16] = {0};

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_udp_socket_create(&server, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_create(&client, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&server, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&client, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&server, &server_local) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&client, &client_local) != C_UdpSocketSuccess) {
        exit_code = 1;
        goto cleanup;
    }

    RecvState server_recv = {&server, {0}, server_buffer, sizeof(server_buffer), {0}};
    SendState client_send = {&client, {0}, request, sizeof(request) - 1, server_local};
    if (galay_coro_spawn(&runtime, recv_entry, &server_recv, 0, &recv_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, send_entry, &client_send, 0, &send_task).code != C_IOResultOk ||
        galay_coro_join(&send_task, 3000).code != C_IOResultOk ||
        galay_coro_join(&recv_task, 3000).code != C_IOResultOk ||
        client_send.result.code != C_IOResultOk ||
        server_recv.result.code != C_IOResultOk ||
        server_recv.result.bytes != sizeof(request) - 1 ||
        memcmp(server_buffer, request, sizeof(request) - 1) != 0 ||
        server_recv.from.port != client_local.port) {
        exit_code = 2;
        goto cleanup;
    }
    if (galay_coro_destroy(&send_task).code != C_IOResultOk ||
        galay_coro_destroy(&recv_task).code != C_IOResultOk) {
        exit_code = 4;
        goto cleanup;
    }

    RecvState client_recv = {&client, {0}, client_buffer, sizeof(client_buffer), {0}};
    SendState server_send = {&server, {0}, response, sizeof(response) - 1, server_recv.from};
    if (galay_coro_spawn(&runtime, recv_entry, &client_recv, 0, &recv_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, send_entry, &server_send, 0, &send_task).code != C_IOResultOk ||
        galay_coro_join(&send_task, 3000).code != C_IOResultOk ||
        galay_coro_join(&recv_task, 3000).code != C_IOResultOk ||
        server_send.result.code != C_IOResultOk ||
        client_recv.result.code != C_IOResultOk ||
        client_recv.result.bytes != sizeof(response) - 1 ||
        memcmp(client_buffer, response, sizeof(response) - 1) != 0 ||
        client_recv.from.port != server_local.port) {
        exit_code = 3;
        goto cleanup;
    }

    if (printf("udp_socket_echo request=%s response=%s server_port=%u client_port=%u\n",
               request,
               client_buffer,
               server_local.port,
               client_local.port) < 0) {
        exit_code = 5;
        goto cleanup;
    }

cleanup:
    if (send_task.task != 0) {
        if (galay_coro_destroy(&send_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 6;
        }
    }
    if (recv_task.task != 0) {
        if (galay_coro_destroy(&recv_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 7;
        }
    }
    if (runtime.runtime != 0) {
        if (client.socket != 0) {
            if (close_socket(&runtime, &client) != 0 && exit_code == 0) {
                exit_code = 8;
            }
        }
        if (server.socket != 0) {
            if (close_socket(&runtime, &server) != 0 && exit_code == 0) {
                exit_code = 9;
            }
        }
    }
    if (client.socket != 0) {
        if (galay_kernel_udp_socket_destroy(&client) != C_UdpSocketSuccess && exit_code == 0) {
            exit_code = 10;
        }
    }
    if (server.socket != 0) {
        if (galay_kernel_udp_socket_destroy(&server) != C_UdpSocketSuccess && exit_code == 0) {
            exit_code = 11;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 12;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 13;
        }
    }
    return exit_code;
}
