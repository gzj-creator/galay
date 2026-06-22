#include <galay/c/galay-kernel/galay_kernel.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum {
    ACCEPT_ITERATIONS = 8
};

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
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

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        (void)close(fd);
        return -1;
    }
    if (connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static int run_accept_once(galay_kernel_runtime_t* runtime)
{
    int result = 0;
    int client_fd = -1;
    galay_kernel_tcp_socket_t* listener = 0;
    galay_kernel_tcp_accept_t* accept = 0;
    galay_kernel_tcp_socket_t* accepted = 0;
    galay_kernel_tcp_host_config_t bind_host = {GALAY_KERNEL_IP_V4, "127.0.0.1", 0};
    galay_kernel_tcp_host_config_t local = {GALAY_KERNEL_IP_V4, 0, 0};
    galay_kernel_tcp_host_config_t peer = {GALAY_KERNEL_IP_V4, 0, 0};

    if (galay_kernel_tcp_socket_create(GALAY_KERNEL_IP_V4, &listener) != GALAY_OK) {
        result = 1;
        goto cleanup;
    }
    if (galay_kernel_tcp_socket_bind(listener, &bind_host) != GALAY_OK) {
        result = 2;
        goto cleanup;
    }
    if (galay_kernel_tcp_socket_listen(listener, 16) != GALAY_OK) {
        result = 3;
        goto cleanup;
    }
    if (galay_kernel_tcp_socket_local_endpoint(listener, &local) != GALAY_OK || local.port == 0) {
        result = 4;
        goto cleanup;
    }
    if (galay_kernel_tcp_accept_start(runtime, listener, &accept) != GALAY_OK) {
        result = 5;
        goto cleanup;
    }

    client_fd = connect_posix_client(local.port);
    if (client_fd < 0) {
        result = 6;
        goto cleanup;
    }
    if (galay_kernel_tcp_accept_join(accept, &accepted, &peer) != GALAY_OK) {
        result = 7;
        goto cleanup;
    }
    if (accepted == 0 || peer.address == 0 || peer.port == 0) {
        result = 8;
        goto cleanup;
    }

cleanup:
    if (accepted != 0) {
        (void)galay_kernel_tcp_socket_destroy(&accepted);
    }
    if (accept != 0) {
        (void)galay_kernel_tcp_accept_destroy(&accept);
    }
    if (listener != 0) {
        (void)galay_kernel_tcp_socket_destroy(&listener);
    }
    if (client_fd >= 0) {
        (void)close(client_fd);
    }
    return result;
}

int main(void)
{
    galay_kernel_runtime_t* runtime = 0;
    galay_kernel_runtime_config_t config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 1;

    if (galay_kernel_runtime_create(&config, &runtime) != GALAY_OK) {
        return 1;
    }
    if (galay_kernel_runtime_start(runtime) != GALAY_OK) {
        (void)galay_kernel_runtime_destroy(&runtime);
        return 2;
    }

    const int64_t start = now_us();
    for (int i = 0; i < ACCEPT_ITERATIONS; ++i) {
        int run_result = run_accept_once(runtime);
        if (run_result != 0) {
            (void)galay_kernel_runtime_stop(runtime);
            (void)galay_kernel_runtime_destroy(&runtime);
            return 10 + run_result;
        }
    }
    const int64_t elapsed = now_us() - start;

    printf("tcp_accept_smoke accepts=%d elapsed_ms=%.3f\n",
           ACCEPT_ITERATIONS,
           elapsed / 1000.0);

    (void)galay_kernel_runtime_stop(runtime);
    return galay_kernel_runtime_destroy(&runtime) == GALAY_OK ? 0 : 3;
}
