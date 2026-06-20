#include <galay/c/galay-kernel/galay_kernel.h>

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    return actual == expected ? 0 : 1;
}

int main(void)
{
    galay_kernel_tcp_host_config_t valid_tcp = {
        GALAY_KERNEL_IP_V4,
        "127.0.0.1",
        0
    };
    galay_kernel_tcp_host_config_t invalid_tcp = {
        GALAY_KERNEL_IP_V4,
        "not-an-ip",
        0
    };
    galay_kernel_udp_host_config_t valid_udp = {
        GALAY_KERNEL_IP_V6,
        "::1",
        0
    };
    galay_kernel_udp_host_config_t invalid_udp = {
        GALAY_KERNEL_IP_V6,
        "127.0.0.1",
        0
    };

    if (expect_status(galay_kernel_tcp_host_config_validate(&valid_tcp), GALAY_OK)) {
        return 1;
    }
    if (expect_status(galay_kernel_tcp_host_config_validate(&invalid_tcp), GALAY_INVALID_ARGUMENT)) {
        return 2;
    }
    if (expect_status(galay_kernel_tcp_host_config_validate(0), GALAY_INVALID_ARGUMENT)) {
        return 3;
    }
    if (expect_status(galay_kernel_udp_host_config_validate(&valid_udp), GALAY_OK)) {
        return 4;
    }
    if (expect_status(galay_kernel_udp_host_config_validate(&invalid_udp), GALAY_INVALID_ARGUMENT)) {
        return 5;
    }

    galay_kernel_tcp_socket_t* tcp = 0;
    galay_kernel_udp_socket_t* udp = 0;
    if (expect_status(galay_kernel_tcp_socket_create(GALAY_KERNEL_IP_V4, &tcp), GALAY_OK)) {
        return 6;
    }
    if (tcp == 0) {
        return 7;
    }
    if (expect_status(galay_kernel_udp_socket_create(GALAY_KERNEL_IP_V4, &udp), GALAY_OK)) {
        return 8;
    }
    if (udp == 0) {
        return 9;
    }
    if (expect_status(galay_kernel_tcp_socket_create((galay_kernel_ip_type_t)99, &tcp), GALAY_INVALID_ARGUMENT)) {
        return 10;
    }
    if (expect_status(galay_kernel_tcp_socket_create(GALAY_KERNEL_IP_V4, 0), GALAY_INVALID_ARGUMENT)) {
        return 11;
    }
    if (expect_status(galay_kernel_tcp_socket_destroy(&tcp), GALAY_OK)) {
        return 12;
    }
    if (tcp != 0) {
        return 13;
    }
    if (expect_status(galay_kernel_tcp_socket_destroy(&tcp), GALAY_OK)) {
        return 14;
    }
    if (expect_status(galay_kernel_udp_socket_destroy(&udp), GALAY_OK)) {
        return 15;
    }
    if (udp != 0) {
        return 16;
    }
    if (expect_status(galay_kernel_udp_socket_destroy(0), GALAY_INVALID_ARGUMENT)) {
        return 17;
    }

    return 0;
}
