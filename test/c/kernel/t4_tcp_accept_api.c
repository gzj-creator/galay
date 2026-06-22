#include <galay/c/galay-kernel/galay_kernel.h>

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    return actual == expected ? 0 : 1;
}

int main(void)
{
    galay_kernel_tcp_accept_t* accept = 0;
    galay_kernel_tcp_socket_t* accepted = 0;
    galay_kernel_tcp_host_config_t peer = {GALAY_KERNEL_IP_V4, 0, 0};
    galay_kernel_tcp_socket_t* listener = 0;
    galay_kernel_tcp_host_config_t host = {GALAY_KERNEL_IP_V4, "127.0.0.1", 0};
    galay_kernel_tcp_host_config_t local = {GALAY_KERNEL_IP_V4, 0, 0};

    if (expect_status(galay_kernel_tcp_accept_start(0, 0, &accept), GALAY_INVALID_ARGUMENT)) {
        return 1;
    }
    if (expect_status(galay_kernel_tcp_accept_wait(0), GALAY_INVALID_ARGUMENT)) {
        return 2;
    }
    if (expect_status(galay_kernel_tcp_accept_join(0, &accepted, &peer), GALAY_INVALID_ARGUMENT)) {
        return 3;
    }
    if (expect_status(galay_kernel_tcp_accept_destroy(0), GALAY_INVALID_ARGUMENT)) {
        return 4;
    }
    if (expect_status(galay_kernel_tcp_accept_destroy(&accept), GALAY_OK)) {
        return 5;
    }
    if (accepted != 0) {
        return 6;
    }
    if (expect_status(galay_kernel_tcp_socket_create(GALAY_KERNEL_IP_V4, &listener), GALAY_OK)) {
        return 7;
    }
    if (listener == 0) {
        return 8;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(0, &host), GALAY_INVALID_ARGUMENT)) {
        return 9;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(listener, 0), GALAY_INVALID_ARGUMENT)) {
        return 10;
    }
    if (expect_status(galay_kernel_tcp_socket_listen(0, 16), GALAY_INVALID_ARGUMENT)) {
        return 11;
    }
    if (expect_status(galay_kernel_tcp_socket_local_endpoint(0, &local), GALAY_INVALID_ARGUMENT)) {
        return 12;
    }
    if (expect_status(galay_kernel_tcp_socket_local_endpoint(listener, 0), GALAY_INVALID_ARGUMENT)) {
        return 13;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(listener, &host), GALAY_OK)) {
        return 14;
    }
    if (expect_status(galay_kernel_tcp_socket_listen(listener, 16), GALAY_OK)) {
        return 15;
    }
    if (expect_status(galay_kernel_tcp_socket_local_endpoint(listener, &local), GALAY_OK)) {
        return 16;
    }
    if (local.address == 0 || local.port == 0) {
        return 17;
    }
    if (expect_status(galay_kernel_tcp_socket_destroy(&listener), GALAY_OK)) {
        return 18;
    }
    if (listener != 0) {
        return 19;
    }
    return 0;
}
