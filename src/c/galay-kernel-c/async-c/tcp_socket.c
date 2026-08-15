#include "tcp_socket.h"
#include "../coro-c/coro_wait.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sendfile.h>
#endif
#include <sys/uio.h>
#include <unistd.h>

enum {
    GALAY_C_TCP_STACK_IOVECS = 16,
};

typedef struct PlatformIovecBuffer {
    struct iovec stack[GALAY_C_TCP_STACK_IOVECS];
    struct iovec* data;
} PlatformIovecBuffer;

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

static C_IOResult make_result_bytes(C_IOResultCode code, size_t bytes, int sys_errno)
{
    return (C_IOResult){code, sys_errno, bytes, 0, NULL};
}

static C_IOResult make_result_value(C_IOResultCode code, int64_t value, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, value, NULL};
}

static int valid_type(C_IPType type)
{
    return type == C_IPTypeIPV4 || type == C_IPTypeIPV6;
}

static int domain_for_type(C_IPType type)
{
    return type == C_IPTypeIPV6 ? AF_INET6 : AF_INET;
}

static int set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_tcp_nodelay(int fd)
{
    const int enabled = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

static C_IOResult platform_iovecs_init(PlatformIovecBuffer* buffer,
                                       const galay_iovec_t* iovecs,
                                       size_t count)
{
    if (buffer == NULL || iovecs == NULL || count == 0 || count > INT_MAX ||
        count > SIZE_MAX / sizeof(struct iovec)) {
        return make_result(C_IOResultInvalid, 0);
    }
#ifdef IOV_MAX
    if (count > IOV_MAX) {
        return make_result(C_IOResultInvalid, 0);
    }
#endif
    buffer->data = buffer->stack;
    if (count > GALAY_C_TCP_STACK_IOVECS) {
        buffer->data = malloc(count * sizeof(struct iovec));
        if (buffer->data == NULL) {
            return make_result(C_IOResultError, ENOMEM);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (iovecs[i].base == NULL && iovecs[i].len != 0) {
            if (buffer->data != buffer->stack) {
                free(buffer->data);
            }
            buffer->data = NULL;
            return make_result(C_IOResultInvalid, 0);
        }
        buffer->data[i].iov_base = iovecs[i].base;
        buffer->data[i].iov_len = iovecs[i].len;
    }
    return make_result(C_IOResultOk, 0);
}

static void platform_iovecs_cleanup(PlatformIovecBuffer* buffer)
{
    if (buffer != NULL && buffer->data != NULL && buffer->data != buffer->stack) {
        free(buffer->data);
    }
    if (buffer != NULL) {
        buffer->data = NULL;
    }
}

static ssize_t platform_sendfile(int socket_fd,
                                 int file_fd,
                                 off_t offset,
                                 size_t count)
{
#if defined(__linux__)
    return sendfile(socket_fd, file_fd, &offset, count);
#elif defined(__APPLE__)
    off_t sent = (off_t)count;
    const int result = sendfile(file_fd, socket_fd, offset, &sent, NULL, 0);
    return result == 0 || sent > 0 ? (ssize_t)sent : -1;
#elif defined(__FreeBSD__)
    off_t sent = 0;
    const int result = sendfile(file_fd, socket_fd, offset, count, NULL, &sent, 0);
    return result == 0 || sent > 0 ? (ssize_t)sent : -1;
#else
    (void)socket_fd;
    (void)file_fd;
    (void)offset;
    (void)count;
    errno = ENOTSUP;
    return -1;
#endif
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

static C_IOResult bind_current_scheduler(galay_c_tcp_socket_t* socket)
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

static C_IOResult initialize_accepted(galay_c_tcp_socket_t* socket,
                                      int fd,
                                      C_IPType type,
                                      galay_c_io_scheduler_t* scheduler)
{
    if (set_nonblocking(fd) != 0 || set_tcp_nodelay(fd) != 0) {
        const int saved_errno = errno;
        if (close(fd) != 0 && saved_errno == 0) {
            return make_result(C_IOResultError, errno);
        }
        return make_result(C_IOResultError, saved_errno);
    }
    memset(socket, 0, sizeof(*socket));
    socket->fd = fd;
    socket->type = type;
    socket->scheduler = scheduler;
    const C_IOResult initialized = galay_c_io_controller_init(&socket->controller, fd, socket);
    if (initialized.code != C_IOResultOk) {
        const int close_result = close(fd);
        socket->fd = -1;
        return close_result == 0 ? initialized : make_result(C_IOResultError, errno);
    }
    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_tcp_socket_create(galay_c_tcp_socket_t* out_socket, C_IPType type)
{
    if (out_socket == NULL || !valid_type(type)) {
        return make_result(C_IOResultInvalid, 0);
    }
    memset(out_socket, 0, sizeof(*out_socket));
    out_socket->fd = -1;
    out_socket->type = type;
    const int fd = socket(domain_for_type(type), SOCK_STREAM, 0);
    if (fd < 0) {
        return make_result(C_IOResultError, errno);
    }
    const C_IOResult initialized = initialize_accepted(out_socket, fd, type, NULL);
    return initialized;
}

C_IOResult galay_c_tcp_socket_bind(galay_c_tcp_socket_t* socket, const C_Host* host)
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

C_IOResult galay_c_tcp_socket_listen(galay_c_tcp_socket_t* socket, int backlog)
{
    if (socket == NULL || socket->fd < 0) {
        return make_result(C_IOResultInvalid, 0);
    }
    return listen(socket->fd, backlog > 0 ? backlog : 128) == 0
        ? make_result(C_IOResultOk, 0)
        : make_result(C_IOResultError, errno);
}

C_IOResult galay_c_tcp_socket_local_endpoint(const galay_c_tcp_socket_t* socket, C_Host* out)
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

C_IOResult galay_c_tcp_socket_set_no_delay(galay_c_tcp_socket_t* socket, int enabled)
{
    if (socket == NULL || socket->fd < 0 || (enabled != 0 && enabled != 1)) {
        return make_result(C_IOResultInvalid, 0);
    }
    return setsockopt(socket->fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0
        ? make_result(C_IOResultOk, 0)
        : make_result(C_IOResultError, errno);
}

C_IOResult galay_c_tcp_socket_accept(galay_c_tcp_socket_t* listener,
                                     galay_c_tcp_socket_t* out_client,
                                     C_Host* out_peer,
                                     int64_t timeout_ms)
{
    if (listener == NULL || out_client == NULL || listener->fd < 0 || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    C_IOResult bound = bind_current_scheduler(listener);
    if (bound.code != C_IOResultOk) {
        return bound;
    }
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    int client_fd = accept(listener->fd, (struct sockaddr*)&peer, &peer_length);
    if (client_fd < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return make_result(C_IOResultError, errno);
    }
    if (client_fd < 0) {
        const C_IOResult waited = galay_c_coro_wait_io(listener->scheduler,
                                                       &listener->controller,
                                                       GALAY_C_EVENT_READ,
                                                       timeout_ms);
        if (waited.code != C_IOResultOk) {
            return waited;
        }
        peer_length = sizeof(peer);
        client_fd = accept(listener->fd, (struct sockaddr*)&peer, &peer_length);
        if (client_fd < 0) {
            return make_result(C_IOResultError, errno);
        }
    }
    C_IOResult initialized = initialize_accepted(out_client, client_fd, listener->type, NULL);
    if (initialized.code != C_IOResultOk) {
        return initialized;
    }
    if (out_peer != NULL && !sockaddr_to_host(&peer, out_peer)) {
        const C_IOResult closed = galay_c_tcp_socket_close(out_client);
        return closed.code == C_IOResultOk
            ? make_result(C_IOResultError, EAFNOSUPPORT)
            : closed;
    }
    return make_result_value(C_IOResultOk, client_fd, 0);
}

C_IOResult galay_c_tcp_socket_connect(galay_c_tcp_socket_t* socket,
                                      const C_Host* host,
                                      int64_t timeout_ms)
{
    struct sockaddr_storage address;
    socklen_t length = 0;
    if (socket == NULL || socket->fd < 0 || host == NULL || host->type != socket->type ||
        timeout_ms < -1 || !host_to_sockaddr(host, &address, &length)) {
        return make_result(C_IOResultInvalid, 0);
    }
    C_IOResult bound = bind_current_scheduler(socket);
    if (bound.code != C_IOResultOk) {
        return bound;
    }
    if (connect(socket->fd, (const struct sockaddr*)&address, length) == 0) {
        return make_result(C_IOResultOk, 0);
    }
    if (errno != EINPROGRESS) {
        return make_result(C_IOResultError, errno);
    }
    const C_IOResult waited = galay_c_coro_wait_io(socket->scheduler, &socket->controller,
                                                    GALAY_C_EVENT_WRITE, timeout_ms);
    if (waited.code != C_IOResultOk) {
        return waited;
    }
    int error = 0;
    socklen_t error_length = sizeof(error);
    if (getsockopt(socket->fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0) {
        return make_result(C_IOResultError, errno);
    }
    return error == 0 ? make_result(C_IOResultOk, 0)
                      : make_result(C_IOResultError, error);
}

C_IOResult galay_c_tcp_socket_recv(galay_c_tcp_socket_t* socket,
                                   char* buffer,
                                   size_t length,
                                   int64_t timeout_ms)
{
    if (socket == NULL || socket->fd < 0 || buffer == NULL || length == 0 || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(socket);
    return bound.code == C_IOResultOk
        ? galay_c_coro_recv_blocking(socket->scheduler, &socket->controller,
                                     buffer, length, timeout_ms)
        : bound;
}

C_IOResult galay_c_tcp_socket_send(galay_c_tcp_socket_t* socket,
                                   const char* buffer,
                                   size_t length,
                                   int64_t timeout_ms)
{
    if (socket == NULL || socket->fd < 0 || buffer == NULL || length == 0 || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(socket);
    return bound.code == C_IOResultOk
        ? galay_c_coro_send_blocking(socket->scheduler, &socket->controller,
                                     buffer, length, timeout_ms)
        : bound;
}

C_IOResult galay_c_tcp_socket_readv(galay_c_tcp_socket_t* socket,
                                    const galay_iovec_t* iovecs,
                                    size_t count,
                                    int64_t timeout_ms)
{
    if (socket == NULL || socket->fd < 0 || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(socket);
    if (bound.code != C_IOResultOk) {
        return bound;
    }
    PlatformIovecBuffer platform = {0};
    C_IOResult prepared = platform_iovecs_init(&platform, iovecs, count);
    if (prepared.code != C_IOResultOk) {
        return prepared;
    }
    ssize_t received = readv(socket->fd, platform.data, (int)count);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        const C_IOResult waited = galay_c_coro_wait_io(socket->scheduler,
                                                       &socket->controller,
                                                       GALAY_C_EVENT_READ,
                                                       timeout_ms);
        if (waited.code != C_IOResultOk) {
            platform_iovecs_cleanup(&platform);
            return waited;
        }
        received = readv(socket->fd, platform.data, (int)count);
    }
    const int saved_errno = received < 0 ? errno : 0;
    platform_iovecs_cleanup(&platform);
    if (received > 0) {
        return make_result_bytes(C_IOResultOk, (size_t)received, 0);
    }
    if (received == 0) {
        return make_result(C_IOResultEof, 0);
    }
    return make_result(C_IOResultError, saved_errno);
}

C_IOResult galay_c_tcp_socket_writev(galay_c_tcp_socket_t* socket,
                                     const galay_iovec_t* iovecs,
                                     size_t count,
                                     int64_t timeout_ms)
{
    if (socket == NULL || socket->fd < 0 || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(socket);
    if (bound.code != C_IOResultOk) {
        return bound;
    }
    PlatformIovecBuffer platform = {0};
    C_IOResult prepared = platform_iovecs_init(&platform, iovecs, count);
    if (prepared.code != C_IOResultOk) {
        return prepared;
    }
    struct msghdr message = {0};
    message.msg_iov = platform.data;
    message.msg_iovlen = count;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
    const int flags = MSG_DONTWAIT;
#endif
    ssize_t sent = sendmsg(socket->fd, &message, flags);
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        const C_IOResult waited = galay_c_coro_wait_io(socket->scheduler,
                                                       &socket->controller,
                                                       GALAY_C_EVENT_WRITE,
                                                       timeout_ms);
        if (waited.code != C_IOResultOk) {
            platform_iovecs_cleanup(&platform);
            return waited;
        }
        sent = sendmsg(socket->fd, &message, flags);
    }
    const int saved_errno = sent < 0 ? errno : 0;
    platform_iovecs_cleanup(&platform);
    return sent >= 0
        ? make_result_bytes(C_IOResultOk, (size_t)sent, 0)
        : make_result(C_IOResultError, saved_errno);
}

C_IOResult galay_c_tcp_socket_sendfile(galay_c_tcp_socket_t* socket,
                                       int file_fd,
                                       int64_t offset,
                                       size_t count,
                                       int64_t timeout_ms)
{
    const off_t platform_offset = (off_t)offset;
    if (socket == NULL || socket->fd < 0 || file_fd < 0 || offset < 0 || count == 0 ||
        timeout_ms < -1 || (int64_t)platform_offset != offset) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(socket);
    if (bound.code != C_IOResultOk) {
        return bound;
    }
    ssize_t sent = platform_sendfile(socket->fd, file_fd, platform_offset, count);
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        const C_IOResult waited = galay_c_coro_wait_io(socket->scheduler,
                                                       &socket->controller,
                                                       GALAY_C_EVENT_WRITE,
                                                       timeout_ms);
        if (waited.code != C_IOResultOk) {
            return waited;
        }
        sent = platform_sendfile(socket->fd, file_fd, platform_offset, count);
    }
    return sent >= 0
        ? make_result_bytes(C_IOResultOk, (size_t)sent, 0)
        : make_result(C_IOResultError, errno);
}

C_IOResult galay_c_tcp_socket_close(galay_c_tcp_socket_t* socket)
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
