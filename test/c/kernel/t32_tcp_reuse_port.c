#include <galay/c/galay-kernel-c/async-c/tcp_socket.h>

#include "socket_test_support.h"

#include <errno.h>
#include <sys/socket.h>

static int close_socket(galay_c_tcp_socket_t* socket, int result, int close_error)
{
    if (socket->fd >= 0 && galay_c_tcp_socket_close(socket).code != C_IOResultOk &&
        result == 0) {
        return close_error;
    }
    return result;
}

int main(void)
{
    if (galay_c_tcp_socket_set_reuse_port(NULL, 1).code != C_IOResultInvalid) {
        return 1;
    }

    const galay_test_socket_capability_t capability =
        galay_test_socket_capability(SOCK_STREAM);
    if (capability == GALAY_TEST_SOCKET_PERMISSION_DENIED) {
        return 125;
    }
    if (capability == GALAY_TEST_SOCKET_PROBE_FAILED) {
        return 126;
    }

    galay_c_tcp_socket_t first = {.fd = -1};
    galay_c_tcp_socket_t second = {.fd = -1};
    C_Host endpoint = {0};
    const C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    int result = 0;

    if (galay_c_tcp_socket_create(&first, C_IPTypeIPV4).code != C_IOResultOk ||
        galay_c_tcp_socket_create(&second, C_IPTypeIPV4).code != C_IOResultOk) {
        result = 2;
        goto cleanup;
    }

    const C_IOResult first_reuse = galay_c_tcp_socket_set_reuse_port(&first, 1);
    if (first_reuse.code == C_IOResultError && first_reuse.sys_errno == ENOTSUP) {
        result = 125;
        goto cleanup;
    }
    if (first_reuse.code != C_IOResultOk ||
        galay_c_tcp_socket_set_reuse_port(&second, 1).code != C_IOResultOk ||
        galay_c_tcp_socket_set_reuse_port(&second, 2).code != C_IOResultInvalid) {
        result = 3;
        goto cleanup;
    }

    if (galay_c_tcp_socket_bind(&first, &bind_host).code != C_IOResultOk ||
        galay_c_tcp_socket_listen(&first, 16).code != C_IOResultOk ||
        galay_c_tcp_socket_local_endpoint(&first, &endpoint).code != C_IOResultOk ||
        endpoint.port == 0 ||
        galay_c_tcp_socket_bind(&second, &endpoint).code != C_IOResultOk ||
        galay_c_tcp_socket_listen(&second, 16).code != C_IOResultOk) {
        result = 4;
    }

cleanup:
    result = close_socket(&second, result, 5);
    result = close_socket(&first, result, 6);
    return result;
}
