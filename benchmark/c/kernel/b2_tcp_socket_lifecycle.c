#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
    TCP_SOCKET_ITERATIONS = 64
};

typedef struct AcceptState {
    atomic_int done;
    atomic_int code;
    galay_kernel_tcp_socket_t socket;
} AcceptState;

typedef struct CloseState {
    atomic_int done;
    atomic_int code;
} CloseState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void on_accept(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    AcceptState* state = (AcceptState*)ctx;
    atomic_store(&state->code, result == 0 ? (int)IOFailed : (int)result->code);
    if (result != 0 && result->code == Success) {
        state->socket = result->socket;
    }
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

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host bind_host = {IPV4, "127.0.0.1", 0};
    C_Host local = {0};
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_tcp_socket_create(&listener, IPV4) != Success ||
        galay_kernel_tcp_socket_bind(&listener, &bind_host) != Success ||
        galay_kernel_tcp_socket_listen(&listener, 64) != Success ||
        galay_kernel_tcp_socket_local_endpoint(&listener, &local) != Success ||
        local.port == 0) {
        return 1;
    }

    const int64_t start = now_us();
    for (int i = 0; i < TCP_SOCKET_ITERATIONS; ++i) {
        AcceptState accept_state;
        atomic_init(&accept_state.done, 0);
        atomic_init(&accept_state.code, (int)IOFailed);
        accept_state.socket.socket = 0;

        CloseState close_state;
        atomic_init(&close_state.done, 0);
        atomic_init(&close_state.code, (int)IOFailed);

        int client_fd = -1;
        if (galay_kernel_tcp_socket_accept(&runtime, &listener, on_accept, &accept_state) != Success) {
            exit_code = 2;
            goto cleanup;
        }

        client_fd = connect_posix_client(local.port);
        if (client_fd < 0 ||
            wait_done(&accept_state.done) != 0 ||
            atomic_load(&accept_state.code) != (int)Success ||
            accept_state.socket.socket == 0) {
            if (client_fd >= 0) {
                close(client_fd);
            }
            exit_code = 3;
            goto cleanup;
        }

        if (galay_kernel_tcp_socket_close(&runtime, &accept_state.socket, on_close, &close_state) != Success ||
            wait_done(&close_state.done) != 0 ||
            atomic_load(&close_state.code) != (int)Success) {
            close(client_fd);
            (void)galay_kernel_tcp_socket_destroy(&accept_state.socket);
            exit_code = 4;
            goto cleanup;
        }

        close(client_fd);
        (void)galay_kernel_tcp_socket_destroy(&accept_state.socket);
    }

cleanup:
    {
        const int64_t elapsed = now_us() - start;
        if (exit_code == 0) {
            const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
            const double ops_per_sec = seconds > 0.0 ? (double)TCP_SOCKET_ITERATIONS / seconds : 0.0;
            printf("tcp_socket_lifecycle iterations=%d elapsed_ms=%.3f ops_per_sec=%.2f\n",
                   TCP_SOCKET_ITERATIONS,
                   (double)elapsed / 1000.0,
                   ops_per_sec);
        }
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
