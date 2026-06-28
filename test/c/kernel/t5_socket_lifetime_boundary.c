#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>

static int expect_runtime_status(C_RuntimeResultCode actual, C_RuntimeResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_socket_status(C_TcpSocketResultCode actual, C_TcpSocketResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_io_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t tcp = {0};

    if (expect_runtime_status(galay_kernel_runtime_create(&config, &runtime),
                              C_RuntimeSuccess)) {
        return 1;
    }
    if (runtime.runtime == 0) {
        return 2;
    }
    if (expect_runtime_status(galay_kernel_runtime_destroy(&runtime), C_RuntimeSuccess)) {
        return 3;
    }
    if (runtime.runtime != 0) {
        return 4;
    }
    if (expect_runtime_status(galay_kernel_runtime_destroy(&runtime), C_RuntimeSuccess)) {
        return 5;
    }
    if (expect_runtime_status(galay_kernel_runtime_destroy(0), C_RuntimeParameterInvalid)) {
        return 6;
    }

    if (expect_socket_status(galay_kernel_tcp_socket_destroy(0),
                             C_TcpSocketParameterInvalid)) {
        return 7;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_destroy(&tcp), C_TcpSocketSuccess)) {
        return 8;
    }
    if (expect_io_code(galay_kernel_tcp_socket_close(0, 0), C_IOResultInvalid)) {
        return 9;
    }
    if (expect_io_code(galay_kernel_tcp_socket_close(&tcp, 0), C_IOResultInvalid)) {
        return 10;
    }

    if (expect_socket_status(galay_kernel_tcp_socket_create(&tcp, C_IPTypeIPV4),
                             C_TcpSocketSuccess)) {
        return 11;
    }
    if (tcp.socket == 0) {
        return 12;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_destroy(&tcp), C_TcpSocketSuccess)) {
        return 13;
    }
    if (tcp.socket != 0) {
        return 14;
    }
    if (expect_socket_status(galay_kernel_tcp_socket_destroy(&tcp), C_TcpSocketSuccess)) {
        return 15;
    }

    return 0;
}
