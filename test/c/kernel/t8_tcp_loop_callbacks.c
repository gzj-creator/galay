#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct AcceptLoopState {
    atomic_int count;
    atomic_int terminal;
    atomic_int errors;
    galay_kernel_tcp_socket_t sockets[2];
} AcceptLoopState;

typedef struct RecvLoopState {
    atomic_int count;
    atomic_int terminal;
    atomic_int errors;
    char* buffer;
    size_t length;
    char messages[2][16];
    size_t sizes[2];
} RecvLoopState;

typedef struct SendLoopState {
    atomic_int count;
    atomic_int terminal;
    atomic_int errors;
    const char* buffer;
    size_t length;
} SendLoopState;

typedef struct CloseState {
    atomic_int done;
    atomic_int code;
} CloseState;

static int expect_status(C_TcpSocketResultCode actual, C_TcpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int wait_at_least(atomic_int* value, int expected)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(value) >= expected) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static int wait_done(atomic_int* done)
{
    return wait_at_least(done, 1);
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

static int send_all(int fd, const char* data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(fd, data + offset, length - offset, 0);
        if (sent <= 0) {
            return 1;
        }
        offset += (size_t)sent;
    }
    return 0;
}

static int read_exact(int fd, char* buffer, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 2000) <= 0 || (pfd.revents & POLLIN) == 0) {
            return 1;
        }
        ssize_t received = recv(fd, buffer + offset, length - offset, 0);
        if (received <= 0) {
            return 1;
        }
        offset += (size_t)received;
    }
    return 0;
}

static int on_accept_loop(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    AcceptLoopState* state = (AcceptLoopState*)ctx;
    if (result == 0 || result->code != Success || result->socket.socket == 0) {
        atomic_store(&state->terminal, 1);
        atomic_fetch_add(&state->errors, 1);
        return 1;
    }

    int index = atomic_fetch_add(&state->count, 1);
    if (index < 2) {
        state->sockets[index] = result->socket;
    } else {
        (void)galay_kernel_tcp_socket_destroy(&result->socket);
    }
    if (index + 1 >= 2) {
        atomic_store(&state->terminal, 1);
        return 1;
    }
    return 0;
}

static int on_recv_loop(galay_kernel_tcp_recv_result_t* result, void* ctx)
{
    RecvLoopState* state = (RecvLoopState*)ctx;
    if (result == 0 || result->code != Success || result->bytes == 0) {
        atomic_store(&state->terminal, 1);
        return 1;
    }
    if (result->buffer != state->buffer || result->length != state->length) {
        atomic_fetch_add(&state->errors, 1);
        return 1;
    }

    int index = atomic_fetch_add(&state->count, 1);
    if (index < 2) {
        size_t copy_length = result->bytes;
        if (copy_length >= sizeof(state->messages[index])) {
            copy_length = sizeof(state->messages[index]) - 1;
        }
        memcpy(state->messages[index], result->buffer, copy_length);
        state->messages[index][copy_length] = '\0';
        state->sizes[index] = result->bytes;
    }
    if (index + 1 >= 2) {
        atomic_store(&state->terminal, 1);
        return 1;
    }
    return 0;
}

static int on_send_loop(galay_kernel_tcp_send_result_t* result, void* ctx)
{
    SendLoopState* state = (SendLoopState*)ctx;
    if (result == 0 || result->code != Success || result->bytes == 0) {
        atomic_store(&state->terminal, 1);
        return 1;
    }
    if (result->buffer != state->buffer || result->length != state->length) {
        atomic_fetch_add(&state->errors, 1);
        return 1;
    }
    int index = atomic_fetch_add(&state->count, 1);
    if (index + 1 >= 2) {
        atomic_store(&state->terminal, 1);
        return 1;
    }
    return 0;
}

static void on_close(C_TcpSocketResultCode code, void* ctx)
{
    CloseState* state = (CloseState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host bind_host = {IPV4, "127.0.0.1", 0};
    C_Host local = {0};
    int recv_client = -1;
    int send_client = -1;
    int exit_code = 0;

    AcceptLoopState accept_state;
    atomic_init(&accept_state.count, 0);
    atomic_init(&accept_state.terminal, 0);
    atomic_init(&accept_state.errors, 0);
    accept_state.sockets[0].socket = 0;
    accept_state.sockets[1].socket = 0;

    char recv_buffer[32] = {0};
    RecvLoopState recv_state;
    memset(&recv_state, 0, sizeof(recv_state));
    atomic_init(&recv_state.count, 0);
    atomic_init(&recv_state.terminal, 0);
    atomic_init(&recv_state.errors, 0);
    recv_state.buffer = recv_buffer;
    recv_state.length = sizeof(recv_buffer);

    const char send_payload[] = "loop";
    SendLoopState send_state;
    memset(&send_state, 0, sizeof(send_state));
    atomic_init(&send_state.count, 0);
    atomic_init(&send_state.terminal, 0);
    atomic_init(&send_state.errors, 0);
    send_state.buffer = send_payload;
    send_state.length = sizeof(send_payload) - 1;

    CloseState close_state;
    atomic_init(&close_state.done, 0);
    atomic_init(&close_state.code, (int)IOFailed);

    if (expect_status(galay_kernel_tcp_socket_accept_loop(0, &listener, on_accept_loop, &accept_state), ParameterInvalid)) {
        return 1;
    }
    if (expect_status(galay_kernel_tcp_socket_accept_loop(&runtime, &listener, 0, &accept_state), ParameterInvalid)) {
        return 2;
    }
    if (expect_status(galay_kernel_tcp_socket_recv_loop(0, &listener, recv_buffer, sizeof(recv_buffer), on_recv_loop, &recv_state), ParameterInvalid)) {
        return 3;
    }
    if (expect_status(galay_kernel_tcp_socket_recv_loop(&runtime, &listener, 0, sizeof(recv_buffer), on_recv_loop, &recv_state), ParameterInvalid)) {
        return 4;
    }
    if (expect_status(galay_kernel_tcp_socket_recv_loop(&runtime, &listener, recv_buffer, 0, on_recv_loop, &recv_state), ParameterInvalid)) {
        return 5;
    }
    if (expect_status(galay_kernel_tcp_socket_recv_loop(&runtime, &listener, recv_buffer, sizeof(recv_buffer), 0, &recv_state), ParameterInvalid)) {
        return 6;
    }
    if (expect_status(galay_kernel_tcp_socket_send_loop(0, &listener, send_payload, sizeof(send_payload) - 1, on_send_loop, &send_state), ParameterInvalid)) {
        return 7;
    }
    if (expect_status(galay_kernel_tcp_socket_send_loop(&runtime, &listener, 0, sizeof(send_payload) - 1, on_send_loop, &send_state), ParameterInvalid)) {
        return 8;
    }
    if (expect_status(galay_kernel_tcp_socket_send_loop(&runtime, &listener, send_payload, 0, on_send_loop, &send_state), ParameterInvalid)) {
        return 9;
    }
    if (expect_status(galay_kernel_tcp_socket_send_loop(&runtime, &listener, send_payload, sizeof(send_payload) - 1, 0, &send_state), ParameterInvalid)) {
        return 10;
    }

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 11;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 12;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_create(&listener, IPV4), Success)) {
        exit_code = 13;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(&listener, &bind_host), Success)) {
        exit_code = 14;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_listen(&listener, 16), Success)) {
        exit_code = 15;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_local_endpoint(&listener, &local), Success)) {
        exit_code = 16;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_accept_loop(&runtime, &listener, on_accept_loop, &accept_state), Success)) {
        exit_code = 17;
        goto cleanup;
    }

    recv_client = connect_posix_client(local.port);
    if (recv_client < 0 || wait_at_least(&accept_state.count, 1) != 0) {
        exit_code = 18;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_recv_loop(
            &runtime,
            &accept_state.sockets[0],
            recv_buffer,
            sizeof(recv_buffer),
            on_recv_loop,
            &recv_state), Success)) {
        exit_code = 19;
        goto cleanup;
    }

    if (send_all(recv_client, "one", 3) != 0 ||
        wait_at_least(&recv_state.count, 1) != 0 ||
        recv_state.sizes[0] != 3 ||
        strcmp(recv_state.messages[0], "one") != 0) {
        exit_code = 20;
        goto cleanup;
    }
    if (send_all(recv_client, "two", 3) != 0 ||
        wait_at_least(&recv_state.count, 2) != 0 ||
        recv_state.sizes[1] != 3 ||
        strcmp(recv_state.messages[1], "two") != 0 ||
        atomic_load(&recv_state.terminal) == 0 ||
        atomic_load(&recv_state.errors) != 0) {
        exit_code = 21;
        goto cleanup;
    }

    send_client = connect_posix_client(local.port);
    if (send_client < 0 || wait_at_least(&accept_state.count, 2) != 0) {
        exit_code = 22;
        goto cleanup;
    }
    if (expect_status(galay_kernel_tcp_socket_send_loop(
            &runtime,
            &accept_state.sockets[1],
            send_payload,
            sizeof(send_payload) - 1,
            on_send_loop,
            &send_state), Success)) {
        exit_code = 23;
        goto cleanup;
    }

    char send_readback[8] = {0};
    if (read_exact(send_client, send_readback, sizeof(send_readback)) != 0 ||
        memcmp(send_readback, "looploop", sizeof(send_readback)) != 0 ||
        wait_at_least(&send_state.count, 2) != 0 ||
        atomic_load(&send_state.terminal) == 0 ||
        atomic_load(&send_state.errors) != 0 ||
        atomic_load(&accept_state.errors) != 0 ||
        atomic_load(&accept_state.terminal) == 0) {
        exit_code = 24;
        goto cleanup;
    }

cleanup:
    if (recv_client >= 0) {
        if (accept_state.sockets[0].socket != 0) {
            (void)wait_done(&recv_state.terminal);
        }
        close(recv_client);
        recv_client = -1;
    }
    if (send_client >= 0) {
        if (accept_state.sockets[1].socket != 0) {
            (void)wait_done(&send_state.terminal);
        }
        close(send_client);
        send_client = -1;
    }
    if (accept_state.sockets[0].socket != 0) {
        (void)galay_kernel_tcp_socket_close(&runtime, &accept_state.sockets[0], on_close, &close_state);
        (void)wait_done(&close_state.done);
        (void)galay_kernel_tcp_socket_destroy(&accept_state.sockets[0]);
    }
    if (accept_state.sockets[1].socket != 0) {
        atomic_store(&close_state.done, 0);
        (void)galay_kernel_tcp_socket_close(&runtime, &accept_state.sockets[1], on_close, &close_state);
        (void)wait_done(&close_state.done);
        (void)galay_kernel_tcp_socket_destroy(&accept_state.sockets[1]);
    }
    if (listener.socket != 0) {
        (void)wait_done(&accept_state.terminal);
        atomic_store(&close_state.done, 0);
        (void)galay_kernel_tcp_socket_close(&runtime, &listener, on_close, &close_state);
        (void)wait_done(&close_state.done);
        (void)galay_kernel_tcp_socket_destroy(&listener);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}
