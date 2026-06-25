#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct AcceptState {
    atomic_int done;
    atomic_int code;
    galay_kernel_tcp_socket_t socket;
} AcceptState;

typedef struct AcceptLoopState {
    atomic_int done;
    atomic_int code;
    atomic_int calls;
    galay_kernel_tcp_socket_t socket;
} AcceptLoopState;

typedef struct RecvState {
    atomic_int done;
    atomic_int code;
    atomic_int bytes;
} RecvState;

static int expect_status(C_TcpSocketResultCode actual, C_TcpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
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

static void on_accept(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    AcceptState* state = (AcceptState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_TcpSocketIOFailed : (int)result->code);
    if (result != 0 && result->code == C_TcpSocketSuccess) {
        state->socket = result->socket;
    }
    atomic_store(&state->done, 1);
}

static int on_accept_loop(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    AcceptLoopState* state = (AcceptLoopState*)ctx;
    atomic_fetch_add(&state->calls, 1);
    atomic_store(&state->code, result == 0 ? (int)C_TcpSocketIOFailed : (int)result->code);
    if (result != 0 && result->code == C_TcpSocketSuccess) {
        state->socket = result->socket;
    }
    atomic_store(&state->done, 1);
    return 1;
}

static void on_recv(galay_kernel_tcp_recv_result_t* result, void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)C_TcpSocketIOFailed : (int)result->code);
    atomic_store(&state->bytes, result == 0 ? -1 : (int)result->bytes);
    atomic_store(&state->done, 1);
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
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int wait_for_readable(int fd)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN) != 0 ? 0 : 1;
}

static int create_listener(galay_kernel_tcp_socket_t* listener, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    if (galay_kernel_tcp_socket_create(listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(listener, 16) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_local_endpoint(listener, local) != C_TcpSocketSuccess ||
        local->port == 0) {
        return 1;
    }
    return 0;
}

static int test_accept_timeout(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    AcceptState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_TcpSocketSuccess);
    state.socket.socket = 0;

    if (create_listener(&listener, &local) != 0) {
        return 101;
    }
    (void)local;

    if (expect_status(galay_kernel_tcp_socket_accept_timeout(0, &listener, 10, on_accept, &state),
                      C_TcpSocketParameterInvalid)) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 102;
    }
    if (expect_status(galay_kernel_tcp_socket_accept_timeout(runtime, &listener, 10, 0, &state),
                      C_TcpSocketParameterInvalid)) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 103;
    }
    if (expect_status(galay_kernel_tcp_socket_accept_timeout(runtime, &listener, 10, on_accept, &state),
                      C_TcpSocketSuccess)) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 104;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_TcpSocketTimeout ||
        state.socket.socket != 0) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 105;
    }

    galay_kernel_tcp_socket_destroy(&listener);
    return 0;
}

static int test_accept_loop_timeout(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    AcceptLoopState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_TcpSocketSuccess);
    atomic_init(&state.calls, 0);
    state.socket.socket = 0;

    if (create_listener(&listener, &local) != 0) {
        return 201;
    }
    (void)local;

    if (expect_status(galay_kernel_tcp_socket_accept_loop_timeout(0, &listener, 10, on_accept_loop, &state),
                      C_TcpSocketParameterInvalid)) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 202;
    }
    if (expect_status(galay_kernel_tcp_socket_accept_loop_timeout(runtime, &listener, 10, 0, &state),
                      C_TcpSocketParameterInvalid)) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 203;
    }
    if (expect_status(galay_kernel_tcp_socket_accept_loop_timeout(runtime, &listener, 10, on_accept_loop, &state),
                      C_TcpSocketSuccess)) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 204;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.calls) != 1 ||
        atomic_load(&state.code) != (int)C_TcpSocketTimeout ||
        state.socket.socket != 0) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 205;
    }

    galay_kernel_tcp_socket_destroy(&listener);
    return 0;
}

static int test_recv_timeout(galay_kernel_runtime_t* runtime)
{
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    int client_fd = -1;
    char recv_buffer[16] = {0};

    AcceptState accept_state;
    atomic_init(&accept_state.done, 0);
    atomic_init(&accept_state.code, (int)C_TcpSocketIOFailed);
    accept_state.socket.socket = 0;

    RecvState recv_state;
    atomic_init(&recv_state.done, 0);
    atomic_init(&recv_state.code, (int)C_TcpSocketSuccess);
    atomic_init(&recv_state.bytes, -1);

    if (create_listener(&listener, &local) != 0) {
        return 301;
    }
    if (galay_kernel_tcp_socket_accept(runtime, &listener, on_accept, &accept_state) != C_TcpSocketSuccess) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 302;
    }
    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        galay_kernel_tcp_socket_destroy(&listener);
        return 303;
    }
    if (wait_done(&accept_state.done) != 0 ||
        atomic_load(&accept_state.code) != (int)C_TcpSocketSuccess ||
        accept_state.socket.socket == 0) {
        close(client_fd);
        galay_kernel_tcp_socket_destroy(&listener);
        return 304;
    }

    if (expect_status(galay_kernel_tcp_socket_recv_timeout(0, &accept_state.socket, recv_buffer,
                                                           sizeof(recv_buffer), 10, on_recv, &recv_state),
                      C_TcpSocketParameterInvalid)) {
        close(client_fd);
        galay_kernel_tcp_socket_destroy(&accept_state.socket);
        galay_kernel_tcp_socket_destroy(&listener);
        return 305;
    }
    if (expect_status(galay_kernel_tcp_socket_recv_timeout(runtime, &accept_state.socket, 0,
                                                           sizeof(recv_buffer), 10, on_recv, &recv_state),
                      C_TcpSocketParameterInvalid)) {
        close(client_fd);
        galay_kernel_tcp_socket_destroy(&accept_state.socket);
        galay_kernel_tcp_socket_destroy(&listener);
        return 306;
    }
    if (expect_status(galay_kernel_tcp_socket_recv_timeout(runtime, &accept_state.socket, recv_buffer,
                                                           0, 10, on_recv, &recv_state),
                      C_TcpSocketParameterInvalid)) {
        close(client_fd);
        galay_kernel_tcp_socket_destroy(&accept_state.socket);
        galay_kernel_tcp_socket_destroy(&listener);
        return 307;
    }
    if (expect_status(galay_kernel_tcp_socket_recv_timeout(runtime, &accept_state.socket, recv_buffer,
                                                           sizeof(recv_buffer), 10, 0, &recv_state),
                      C_TcpSocketParameterInvalid)) {
        close(client_fd);
        galay_kernel_tcp_socket_destroy(&accept_state.socket);
        galay_kernel_tcp_socket_destroy(&listener);
        return 308;
    }
    if (expect_status(galay_kernel_tcp_socket_recv_timeout(runtime, &accept_state.socket, recv_buffer,
                                                           sizeof(recv_buffer), 10, on_recv, &recv_state),
                      C_TcpSocketSuccess)) {
        close(client_fd);
        galay_kernel_tcp_socket_destroy(&accept_state.socket);
        galay_kernel_tcp_socket_destroy(&listener);
        return 309;
    }
    if (wait_done(&recv_state.done) != 0 ||
        atomic_load(&recv_state.code) != (int)C_TcpSocketTimeout ||
        atomic_load(&recv_state.bytes) != 0) {
        close(client_fd);
        galay_kernel_tcp_socket_destroy(&accept_state.socket);
        galay_kernel_tcp_socket_destroy(&listener);
        return 310;
    }
    if (wait_for_readable(client_fd) == 0) {
        close(client_fd);
        galay_kernel_tcp_socket_destroy(&accept_state.socket);
        galay_kernel_tcp_socket_destroy(&listener);
        return 311;
    }

    close(client_fd);
    galay_kernel_tcp_socket_destroy(&accept_state.socket);
    galay_kernel_tcp_socket_destroy(&listener);
    return 0;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    int result = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        galay_kernel_runtime_destroy(&runtime);
        return 2;
    }

    result = test_accept_timeout(&runtime);
    if (result == 0) {
        result = test_accept_loop_timeout(&runtime);
    }
    if (result == 0) {
        result = test_recv_timeout(&runtime);
    }

    galay_kernel_runtime_stop(&runtime);
    galay_kernel_runtime_destroy(&runtime);
    return result;
}
