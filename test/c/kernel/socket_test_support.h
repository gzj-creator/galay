#ifndef GALAY_TEST_C_KERNEL_SOCKET_TEST_SUPPORT_H
#define GALAY_TEST_C_KERNEL_SOCKET_TEST_SUPPORT_H

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

typedef enum galay_test_socket_capability {
    GALAY_TEST_SOCKET_AVAILABLE,
    GALAY_TEST_SOCKET_PERMISSION_DENIED,
    GALAY_TEST_SOCKET_PROBE_FAILED,
} galay_test_socket_capability_t;

static inline galay_test_socket_capability_t galay_test_socket_capability(int type)
{
    const int fd = socket(AF_INET, type, 0);
    if (fd < 0) {
        return errno == EPERM || errno == EACCES
            ? GALAY_TEST_SOCKET_PERMISSION_DENIED
            : GALAY_TEST_SOCKET_PROBE_FAILED;
    }
    return close(fd) == 0
        ? GALAY_TEST_SOCKET_AVAILABLE
        : GALAY_TEST_SOCKET_PROBE_FAILED;
}

#endif
