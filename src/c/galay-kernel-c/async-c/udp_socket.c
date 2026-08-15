#include "udp_socket.h"
#include "../coro-c/coro_wait.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

static C_IOResult make_result_bytes(C_IOResultCode code, size_t bytes, int sys_errno)
{
    return (C_IOResult){code, sys_errno, bytes, 0, NULL};
}

static int valid_type(C_IPType type)
{
    return type == C_IPTypeIPV4 || type == C_IPTypeIPV6;
}

static int set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int host_to_sockaddr(const C_Host* host,
                            struct sockaddr_storage* storage,
                            socklen_t* length)
{
    if (host == NULL || storage == NULL || length == NULL || !valid_type(host->type) ||
        host->address[0] == '\0') {
        return 0;
    }
    memset(storage, 0, sizeof(*storage));
    if (host->type == C_IPTypeIPV4) {
        struct sockaddr_in* address = (struct sockaddr_in*)storage;
        address->sin_family = AF_INET;
        address->sin_port = htons(host->port);
        if (strcmp(host->address, "0.0.0.0") == 0) {
            address->sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, host->address, &address->sin_addr) != 1) {
            return 0;
        }
        *length = sizeof(*address);
        return 1;
    }
    struct sockaddr_in6* address = (struct sockaddr_in6*)storage;
    address->sin6_family = AF_INET6;
    address->sin6_port = htons(host->port);
    if (strcmp(host->address, "::") == 0) {
        address->sin6_addr = in6addr_any;
    } else if (inet_pton(AF_INET6, host->address, &address->sin6_addr) != 1) {
        return 0;
    }
    *length = sizeof(*address);
    return 1;
}

static int sockaddr_to_host(const struct sockaddr_storage* storage, C_Host* host)
{
    if (storage == NULL || host == NULL) {
        return 0;
    }
    memset(host, 0, sizeof(*host));
    if (storage->ss_family == AF_INET) {
        const struct sockaddr_in* address = (const struct sockaddr_in*)storage;
        host->type = C_IPTypeIPV4;
        host->port = ntohs(address->sin_port);
        return inet_ntop(AF_INET, &address->sin_addr, host->address,
                         sizeof(host->address)) != NULL;
    }
    if (storage->ss_family == AF_INET6) {
        const struct sockaddr_in6* address = (const struct sockaddr_in6*)storage;
        host->type = C_IPTypeIPV6;
        host->port = ntohs(address->sin6_port);
        return inet_ntop(AF_INET6, &address->sin6_addr, host->address,
                         sizeof(host->address)) != NULL;
    }
    return 0;
}

static C_IOResult bind_current_scheduler(galay_c_udp_socket_t* socket)
{
    galay_c_io_scheduler_t* const current = galay_c_io_scheduler_current();
    if (socket == NULL || current == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    if (socket->scheduler != NULL && socket->scheduler != current) {
        return make_result(C_IOResultInvalid, 0);
    }
    socket->scheduler = current;
    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_udp_socket_create(galay_c_udp_socket_t* out_socket, C_IPType type)
{
    if (out_socket == NULL || !valid_type(type)) {
        return make_result(C_IOResultInvalid, 0);
    }
    memset(out_socket, 0, sizeof(*out_socket));
    out_socket->fd = -1;
    out_socket->type = type;
    const int fd = socket(type == C_IPTypeIPV6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return make_result(C_IOResultError, errno);
    }
    if (set_nonblocking(fd) != 0) {
        const int saved_errno = errno;
        return close(fd) == 0 ? make_result(C_IOResultError, saved_errno)
                              : make_result(C_IOResultError, errno);
    }
    out_socket->fd = fd;
    const C_IOResult initialized =
        galay_c_io_controller_init(&out_socket->controller, fd, out_socket);
    if (initialized.code != C_IOResultOk) {
        const int close_result = close(fd);
        out_socket->fd = -1;
        return close_result == 0 ? initialized : make_result(C_IOResultError, errno);
    }
    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_udp_socket_bind(galay_c_udp_socket_t* socket, const C_Host* host)
{
    struct sockaddr_storage address;
    socklen_t length = 0;
    if (socket == NULL || socket->fd < 0 || host == NULL || host->type != socket->type ||
        !host_to_sockaddr(host, &address, &length)) {
        return make_result(C_IOResultInvalid, 0);
    }
    const int reuse = 1;
    if (setsockopt(socket->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        return make_result(C_IOResultError, errno);
    }
    return bind(socket->fd, (const struct sockaddr*)&address, length) == 0
        ? make_result(C_IOResultOk, 0)
        : make_result(C_IOResultError, errno);
}

C_IOResult galay_c_udp_socket_local_endpoint(const galay_c_udp_socket_t* socket, C_Host* out)
{
    if (socket == NULL || socket->fd < 0 || out == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    if (getsockname(socket->fd, (struct sockaddr*)&address, &length) != 0) {
        return make_result(C_IOResultError, errno);
    }
    return sockaddr_to_host(&address, out)
        ? make_result(C_IOResultOk, 0)
        : make_result(C_IOResultError, EAFNOSUPPORT);
}

C_IOResult galay_c_udp_socket_recvfrom(galay_c_udp_socket_t* socket,
                                       char* buffer,
                                       size_t length,
                                       C_Host* out_peer,
                                       int64_t timeout_ms)
{
    if (socket == NULL || socket->fd < 0 || buffer == NULL || length == 0 ||
        timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(socket);
    if (bound.code != C_IOResultOk) {
        return bound;
    }

    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    ssize_t received = recvfrom(socket->fd, buffer, length, MSG_DONTWAIT,
                                (struct sockaddr*)&peer, &peer_length);
    if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return make_result(C_IOResultError, errno);
    }
    if (received < 0) {
        const C_IOResult waited = galay_c_coro_wait_io(socket->scheduler,
                                                       &socket->controller,
                                                       GALAY_C_EVENT_READ,
                                                       timeout_ms);
        if (waited.code != C_IOResultOk) {
            return waited;
        }
        peer_length = sizeof(peer);
        received = recvfrom(socket->fd, buffer, length, MSG_DONTWAIT,
                            (struct sockaddr*)&peer, &peer_length);
        if (received < 0) {
            return make_result(C_IOResultError, errno);
        }
    }
    if (out_peer != NULL && !sockaddr_to_host(&peer, out_peer)) {
        return make_result(C_IOResultError, EAFNOSUPPORT);
    }
    return make_result_bytes(C_IOResultOk, (size_t)received, 0);
}

C_IOResult galay_c_udp_socket_sendto(galay_c_udp_socket_t* socket,
                                     const char* buffer,
                                     size_t length,
                                     const C_Host* peer,
                                     int64_t timeout_ms)
{
    struct sockaddr_storage address;
    socklen_t address_length = 0;
    if (socket == NULL || socket->fd < 0 || buffer == NULL || length == 0 ||
        peer == NULL || peer->type != socket->type || timeout_ms < -1 ||
        !host_to_sockaddr(peer, &address, &address_length)) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(socket);
    if (bound.code != C_IOResultOk) {
        return bound;
    }

    ssize_t sent = sendto(socket->fd, buffer, length, MSG_DONTWAIT,
                          (const struct sockaddr*)&address, address_length);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return make_result(C_IOResultError, errno);
    }
    if (sent < 0) {
        const C_IOResult waited = galay_c_coro_wait_io(socket->scheduler,
                                                       &socket->controller,
                                                       GALAY_C_EVENT_WRITE,
                                                       timeout_ms);
        if (waited.code != C_IOResultOk) {
            return waited;
        }
        sent = sendto(socket->fd, buffer, length, MSG_DONTWAIT,
                      (const struct sockaddr*)&address, address_length);
        if (sent < 0) {
            return make_result(C_IOResultError, errno);
        }
    }
    return make_result_bytes(C_IOResultOk, (size_t)sent, 0);
}

C_IOResult galay_c_udp_socket_close(galay_c_udp_socket_t* socket)
{
    if (socket == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    if (socket->fd < 0) {
        return make_result(C_IOResultOk, 0);
    }
    C_IOResult first_error = make_result(C_IOResultOk, 0);
    if (socket->scheduler != NULL) {
        if (atomic_load_explicit(&socket->controller.read_slot, memory_order_acquire) != NULL) {
            const C_IOResult cancelled = galay_c_coro_cancel_io(
                socket->scheduler, &socket->controller, GALAY_C_EVENT_READ);
            if (cancelled.code != C_IOResultCancelled && first_error.code == C_IOResultOk) {
                first_error = cancelled;
            }
        }
        if (atomic_load_explicit(&socket->controller.write_slot, memory_order_acquire) != NULL) {
            const C_IOResult cancelled = galay_c_coro_cancel_io(
                socket->scheduler, &socket->controller, GALAY_C_EVENT_WRITE);
            if (cancelled.code != C_IOResultCancelled && first_error.code == C_IOResultOk) {
                first_error = cancelled;
            }
        }
        if (atomic_load_explicit(&socket->controller.registered_events,
                                 memory_order_acquire) != GALAY_C_EVENT_NONE) {
            const C_IOResult unregistered =
                galay_c_io_scheduler_unregister(socket->scheduler, &socket->controller);
            if (unregistered.code != C_IOResultOk && first_error.code == C_IOResultOk) {
                first_error = unregistered;
            }
        }
    }
    const C_IOResult cleaned = galay_c_io_controller_cleanup(&socket->controller);
    if (cleaned.code != C_IOResultOk && first_error.code == C_IOResultOk) {
        first_error = cleaned;
    }
    if (close(socket->fd) != 0 && first_error.code == C_IOResultOk) {
        first_error = make_result(C_IOResultError, errno);
    }
    socket->fd = -1;
    socket->scheduler = NULL;
    return first_error;
}
