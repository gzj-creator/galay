#include <galay/c/galay-kernel/galay_kernel.h>

#include <stdint.h>

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    return actual == expected ? 0 : 1;
}

int main(void)
{
    galay_kernel_runtime_config_t config = galay_kernel_runtime_config_default();
    galay_kernel_runtime_t* runtime = 0;
    galay_kernel_tcp_accept_t* accept = 0;
    galay_kernel_tcp_socket_t* accepted = (galay_kernel_tcp_socket_t*)(uintptr_t)1;
    galay_kernel_tcp_socket_t* tcp = 0;
    galay_kernel_udp_socket_t* udp = 0;

    if (expect_status(galay_kernel_runtime_create(&config, &runtime), GALAY_OK)) {
        return 1;
    }
    if (runtime == 0) {
        return 2;
    }
    if (expect_status(galay_kernel_runtime_destroy(&runtime), GALAY_OK)) {
        return 3;
    }
    if (runtime != 0) {
        return 4;
    }
    if (expect_status(galay_kernel_runtime_destroy(&runtime), GALAY_OK)) {
        return 5;
    }

    if (expect_status(galay_kernel_tcp_accept_join(0, &accepted, 0), GALAY_INVALID_ARGUMENT)) {
        return 6;
    }
    if (accepted != 0) {
        return 7;
    }
    if (expect_status(galay_kernel_tcp_accept_destroy(&accept), GALAY_OK)) {
        return 8;
    }
    if (expect_status(galay_kernel_tcp_accept_destroy(&accept), GALAY_OK)) {
        return 9;
    }

    if (expect_status(galay_kernel_tcp_socket_create(GALAY_KERNEL_IP_V4, &tcp), GALAY_OK)) {
        return 10;
    }
    if (expect_status(galay_kernel_udp_socket_create(GALAY_KERNEL_IP_V4, &udp), GALAY_OK)) {
        return 11;
    }
    if (expect_status(galay_kernel_tcp_socket_destroy(&tcp), GALAY_OK)) {
        return 12;
    }
    if (expect_status(galay_kernel_tcp_socket_destroy(&tcp), GALAY_OK)) {
        return 13;
    }
    if (expect_status(galay_kernel_udp_socket_destroy(&udp), GALAY_OK)) {
        return 14;
    }
    if (expect_status(galay_kernel_udp_socket_destroy(&udp), GALAY_OK)) {
        return 15;
    }

    return 0;
}
