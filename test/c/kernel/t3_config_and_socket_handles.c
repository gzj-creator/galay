#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

static int expect_status(C_TcpSocketResultCode actual, C_TcpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

int main(void)
{
    galay_kernel_tcp_socket_t tcp = {0};
    C_Host invalid_host = {IPV4, "not-an-ip", 0};
    C_Host bind_host = {IPV4, "127.0.0.1", 0};

    if (expect_status(galay_kernel_tcp_socket_create(&tcp, IPV4), Success)) {
        return 1;
    }
    if (tcp.socket == 0) {
        return 2;
    }
    if (expect_status(galay_kernel_tcp_socket_create(&tcp, (C_IPType)99), ParameterInvalid)) {
        return 3;
    }
    if (expect_status(galay_kernel_tcp_socket_create(0, IPV4), ParameterInvalid)) {
        return 4;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(0, &bind_host), ParameterInvalid)) {
        return 5;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(&tcp, 0), ParameterInvalid)) {
        return 6;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(&tcp, &invalid_host), ParameterInvalid)) {
        return 7;
    }
    if (expect_status(galay_kernel_tcp_socket_bind(&tcp, &bind_host), Success)) {
        return 8;
    }
    if (expect_status(galay_kernel_tcp_socket_destroy(&tcp), Success)) {
        return 9;
    }

    return 0;
}
