#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct RecvState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
    C_Host from;
} RecvState;

typedef struct SendState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
} SendState;

typedef struct CloseState {
    atomic_int done;
    atomic_int code;
} CloseState;

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static void on_recv(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? 0 : (int)result->bytes);
    if (result != 0) {
        state->from = result->from;
    }
    atomic_store(&state->done, 1);
}

static void on_send(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    SendState* state = (SendState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_UdpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? 0 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_close(C_UdpSocketResultCode code, void* ctx)
{
    CloseState* state = (CloseState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

static int close_socket(galay_kernel_runtime_t* runtime, galay_kernel_udp_socket_t* socket)
{
    CloseState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_UdpSocketIOFailed);
    if (socket->socket == 0) {
        return 0;
    }
    if (galay_kernel_udp_socket_close(runtime, socket, on_close, &state) != C_UdpSocketSuccess) {
        return 1;
    }
    return wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_UdpSocketSuccess;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_udp_socket_t server = {0};
    galay_kernel_udp_socket_t client = {0};
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host server_local = {0};
    C_Host client_local = {0};
    int exit_code = 0;

    const char request[] = "ping";
    const char response[] = "pong";
    char server_buffer[16] = {0};
    char client_buffer[16] = {0};

    RecvState server_recv;
    memset(&server_recv, 0, sizeof(server_recv));
    atomic_init(&server_recv.done, 0);
    atomic_init(&server_recv.code, (int)C_UdpSocketIOFailed);
    atomic_init(&server_recv.bytes, 0);

    SendState client_send;
    atomic_init(&client_send.done, 0);
    atomic_init(&client_send.code, (int)C_UdpSocketIOFailed);
    atomic_init(&client_send.bytes, 0);

    RecvState client_recv;
    memset(&client_recv, 0, sizeof(client_recv));
    atomic_init(&client_recv.done, 0);
    atomic_init(&client_recv.code, (int)C_UdpSocketIOFailed);
    atomic_init(&client_recv.bytes, 0);

    SendState server_send;
    atomic_init(&server_send.done, 0);
    atomic_init(&server_send.code, (int)C_UdpSocketIOFailed);
    atomic_init(&server_send.bytes, 0);

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

    if (galay_kernel_udp_socket_recvfrom(&runtime, &server,
            server_buffer, sizeof(server_buffer), on_recv, &server_recv) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_sendto(&runtime, &client,
            request, sizeof(request) - 1, &server_local, on_send, &client_send) != C_UdpSocketSuccess) {
        exit_code = 2;
        goto cleanup;
    }
    if (wait_done(&client_send.done) != 0 ||
        wait_done(&server_recv.done) != 0 ||
        atomic_load(&client_send.code) != (int)C_UdpSocketSuccess ||
        atomic_load(&server_recv.code) != (int)C_UdpSocketSuccess ||
        atomic_load(&server_recv.bytes) != (int)(sizeof(request) - 1) ||
        memcmp(server_buffer, request, sizeof(request) - 1) != 0 ||
        server_recv.from.port != client_local.port) {
        exit_code = 3;
        goto cleanup;
    }

    if (galay_kernel_udp_socket_recvfrom(&runtime, &client,
            client_buffer, sizeof(client_buffer), on_recv, &client_recv) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_sendto(&runtime, &server,
            response, sizeof(response) - 1, &server_recv.from, on_send, &server_send) != C_UdpSocketSuccess) {
        exit_code = 4;
        goto cleanup;
    }
    if (wait_done(&server_send.done) != 0 ||
        wait_done(&client_recv.done) != 0 ||
        atomic_load(&server_send.code) != (int)C_UdpSocketSuccess ||
        atomic_load(&client_recv.code) != (int)C_UdpSocketSuccess ||
        atomic_load(&client_recv.bytes) != (int)(sizeof(response) - 1) ||
        memcmp(client_buffer, response, sizeof(response) - 1) != 0 ||
        client_recv.from.port != server_local.port) {
        exit_code = 5;
        goto cleanup;
    }

    printf("udp_socket_echo request=%s response=%s server_port=%u client_port=%u\n",
           request,
           client_buffer,
           server_local.port,
           client_local.port);

cleanup:
    if (runtime.runtime != 0) {
        if (client.socket != 0) {
            (void)close_socket(&runtime, &client);
        }
        if (server.socket != 0) {
            (void)close_socket(&runtime, &server);
        }
    }
    if (client.socket != 0) {
        (void)galay_kernel_udp_socket_destroy(&client);
    }
    if (server.socket != 0) {
        (void)galay_kernel_udp_socket_destroy(&server);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}
