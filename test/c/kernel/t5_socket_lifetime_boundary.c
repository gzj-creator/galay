#include <galay/c/galay-kernel-c/async-c/tcp_socket.h>
#include <galay/c/galay-kernel-c/core-c/runtime.h>

#include "socket_test_support.h"

static int expect_runtime_status(C_RuntimeResultCode actual, C_RuntimeResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_io_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

int main(void)
{
    const galay_test_socket_capability_t socket_capability =
        galay_test_socket_capability(SOCK_STREAM);
    if (socket_capability == GALAY_TEST_SOCKET_PERMISSION_DENIED) {
        return 125;
    }
    if (socket_capability == GALAY_TEST_SOCKET_PROBE_FAILED) {
        return 126;
    }

    C_RuntimeConfig config = galay_c_runtime_config_default();
    galay_c_runtime_t runtime = {0};
    galay_c_tcp_socket_t tcp = {.fd = -1};

    if (expect_runtime_status(galay_c_runtime_create(&config, &runtime),
                              C_RuntimeSuccess)) {
        return 1;
    }
    if (runtime.runtime == 0) {
        return 2;
    }
    if (expect_runtime_status(galay_c_runtime_destroy(&runtime), C_RuntimeSuccess)) {
        return 3;
    }
    if (runtime.runtime != 0) {
        return 4;
    }
    if (expect_runtime_status(galay_c_runtime_destroy(&runtime), C_RuntimeSuccess)) {
        return 5;
    }
    if (expect_runtime_status(galay_c_runtime_destroy(0), C_RuntimeParameterInvalid)) {
        return 6;
    }

    if (expect_io_code(galay_c_tcp_socket_close(NULL), C_IOResultInvalid)) {
        return 7;
    }
    if (expect_io_code(galay_c_tcp_socket_close(&tcp), C_IOResultOk)) {
        return 8;
    }

    if (expect_io_code(galay_c_tcp_socket_create(&tcp, C_IPTypeIPV4), C_IOResultOk)) {
        return 11;
    }
    if (tcp.fd < 0) {
        return 12;
    }
    if (expect_io_code(galay_c_tcp_socket_close(&tcp), C_IOResultOk)) {
        return 13;
    }
    if (tcp.fd != -1) {
        return 14;
    }
    if (expect_io_code(galay_c_tcp_socket_close(&tcp), C_IOResultOk)) {
        return 15;
    }

    return 0;
}
