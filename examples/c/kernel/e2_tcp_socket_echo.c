#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct AcceptState {
    atomic_int done;
    atomic_int code;
    galay_kernel_tcp_socket_t socket;
} AcceptState;

typedef struct RecvState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
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

static void on_accept(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    AcceptState* state = (AcceptState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_TcpSocketIOFailed : (int)result->code);
    if (result != 0 && result->code == C_TcpSocketSuccess) {
        state->socket = result->socket;
    }
    atomic_store(&state->done, 1);
}

static void on_recv(galay_kernel_tcp_recv_result_t* result, void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_TcpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? 0 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_send(galay_kernel_tcp_send_result_t* result, void* ctx)
{
    SendState* state = (SendState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_TcpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? 0 : (int)result->bytes);
    atomic_store(&state->done, 1);
}

static void on_close(C_TcpSocketResultCode code, void* ctx)
{
    CloseState* state = (CloseState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

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

static int connect_posix_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
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
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host local = {0};
    int client_fd = -1;
    int exit_code = 0;

    const char request[] = "ping";
    const char response[] = "pong";
    char recv_buffer[16] = {0};
    char client_buffer[16] = {0};

    AcceptState accept_state;
    atomic_init(&accept_state.done, 0);
    atomic_init(&accept_state.code, (int)C_TcpSocketIOFailed);
    accept_state.socket.socket = 0;

    RecvState recv_state;
    atomic_init(&recv_state.done, 0);
    atomic_init(&recv_state.code, (int)C_TcpSocketIOFailed);
    atomic_init(&recv_state.bytes, 0);

    SendState send_state;
    atomic_init(&send_state.done, 0);
    atomic_init(&send_state.code, (int)C_TcpSocketIOFailed);
    atomic_init(&send_state.bytes, 0);

    CloseState close_state;
    atomic_init(&close_state.done, 0);
    atomic_init(&close_state.code, (int)C_TcpSocketIOFailed);

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_tcp_socket_create(&listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(&listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&listener, 16) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_local_endpoint(&listener, &local) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_accept(&runtime, &listener, on_accept, &accept_state) != C_TcpSocketSuccess) {
        exit_code = 1;
        goto cleanup;
    }

    client_fd = connect_posix_client(local.port);
    if (client_fd < 0 || wait_done(&accept_state.done) != 0 ||
        atomic_load(&accept_state.code) != (int)C_TcpSocketSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    if (galay_kernel_tcp_socket_recv(&runtime, &accept_state.socket,
            recv_buffer, sizeof(recv_buffer), on_recv, &recv_state) != C_TcpSocketSuccess ||
        send(client_fd, request, sizeof(request) - 1, 0) != (ssize_t)(sizeof(request) - 1) ||
        wait_done(&recv_state.done) != 0 ||
        atomic_load(&recv_state.code) != (int)C_TcpSocketSuccess ||
        memcmp(recv_buffer, request, sizeof(request) - 1) != 0) {
        exit_code = 3;
        goto cleanup;
    }

    if (galay_kernel_tcp_socket_send(&runtime, &accept_state.socket,
            response, sizeof(response) - 1, on_send, &send_state) != C_TcpSocketSuccess ||
        wait_done(&send_state.done) != 0 ||
        atomic_load(&send_state.code) != (int)C_TcpSocketSuccess ||
        wait_and_read(client_fd, client_buffer, sizeof(client_buffer)) != (int)(sizeof(response) - 1) ||
        memcmp(client_buffer, response, sizeof(response) - 1) != 0) {
        exit_code = 4;
        goto cleanup;
    }

    if (galay_kernel_tcp_socket_close(&runtime, &accept_state.socket, on_close, &close_state) != C_TcpSocketSuccess ||
        wait_done(&close_state.done) != 0 ||
        atomic_load(&close_state.code) != (int)C_TcpSocketSuccess) {
        exit_code = 5;
        goto cleanup;
    }

    printf("tcp_socket_echo request=%s response=%s port=%u\n", request, client_buffer, local.port);

cleanup:
    if (client_fd >= 0) {
        close(client_fd);
    }
    if (accept_state.socket.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&accept_state.socket);
    }
    if (listener.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}
