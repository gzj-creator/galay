#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <arpa/inet.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct AcceptState {
    atomic_int done;
    atomic_int code;
    galay_kernel_tcp_socket_t socket;
    C_Host peer;
} AcceptState;

static int expect_status(C_TcpSocketResultCode actual, C_TcpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void on_accept(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    AcceptState* state = (AcceptState*)ctx;
    if (result == 0) {
        atomic_store(&state->code, (int)C_TcpSocketIOFailed);
        atomic_store(&state->done, 1);
        return;
    }

    atomic_store(&state->code, (int)result->code);
    state->peer = result->peer;
    if (result->code == C_TcpSocketSuccess) {
        state->socket = result->socket;
    }
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

    AcceptState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_TcpSocketIOFailed);
    state.socket.socket = 0;
    memset(&state.peer, 0, sizeof(state.peer));

    if (expect_status(galay_kernel_tcp_socket_accept(0, &listener, on_accept, &state), C_TcpSocketParameterInvalid)) {
        return 1;
    }
    if (expect_status(galay_kernel_tcp_socket_accept(&runtime, &listener, 0, &state), C_TcpSocketParameterInvalid)) {
        return 2;
    }

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 3;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 4;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_create(&listener, C_IPTypeIPV4), C_TcpSocketSuccess)) {
        exit_code = 5;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(&listener, &bind_host), C_TcpSocketSuccess)) {
        exit_code = 6;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_listen(&listener, 16), C_TcpSocketSuccess)) {
        exit_code = 7;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_local_endpoint(&listener, &local), C_TcpSocketSuccess) || local.port == 0) {
        exit_code = 8;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_accept(&runtime, &listener, on_accept, &state), C_TcpSocketSuccess)) {
        exit_code = 9;
        goto cleanup;
    }

    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        exit_code = 10;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_TcpSocketSuccess ||
        state.socket.socket == 0 ||
        state.peer.type != C_IPTypeIPV4 ||
        state.peer.address[0] == '\0' ||
        state.peer.port == 0) {
        exit_code = 11;
        goto cleanup;
    }

cleanup:
    if (client_fd >= 0) {
        close(client_fd);
    }
    if (state.socket.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&state.socket);
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
