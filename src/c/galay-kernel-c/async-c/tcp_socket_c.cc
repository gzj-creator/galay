#include "tcp_socket_c.h"

#include "../../../cpp/galay-kernel/async/tcp_socket.h"
#include "../coro-c/coro_task_internal.hpp"
#include "../coro-c/coro_wait_c.h"
#include <galay/c/galay-bridge-c/coro-c/c_coro_tcp_bridge.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <limits>
#include <netinet/in.h>
#include <new>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace
{

static_assert(C_HOST_ADDRESS_MAX_LENGTH == GALAY_CORE_CORO_HOST_ADDRESS_MAX_LENGTH);

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

bool is_valid_c_ip_type(C_IPType ip_type)
{
    return ip_type == C_IPTypeIPV4 || ip_type == C_IPTypeIPV6;
}

galay::kernel::IPType from_c_ip_type_to_cpp_ip_type(C_IPType ip_type)
{
    switch (ip_type) {
    case C_IPTypeIPV4:
        return galay::kernel::IPType::IPV4;
    case C_IPTypeIPV6:
        return galay::kernel::IPType::IPV6;
    }
    return galay::kernel::IPType::IPV4;
}

std::string from_c_host_address_to_string(const C_Host& host)
{
    const void* end = std::memchr(host.address, '\0', sizeof(host.address));
    const auto length = end == nullptr
        ? sizeof(host.address)
        : static_cast<size_t>(static_cast<const char*>(end) - host.address);
    return std::string(host.address, length);
}

galay::kernel::Host from_c_host_to_cpp_host(const C_Host& host)
{
    return galay::kernel::Host(
        from_c_ip_type_to_cpp_ip_type(host.type),
        from_c_host_address_to_string(host),
        host.port);
}

bool assign_c_host_from_sockaddr(const sockaddr_storage& storage, C_Host* endpoint)
{
    C_Host out{};
    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &addr->sin_addr, out.address, sizeof(out.address)) == nullptr) {
            return false;
        }
        out.type = C_IPTypeIPV4;
        out.port = ntohs(addr->sin_port);
        *endpoint = out;
        return true;
    }
    if (storage.ss_family == AF_INET6) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &addr->sin6_addr, out.address, sizeof(out.address)) == nullptr) {
            return false;
        }
        out.type = C_IPTypeIPV6;
        out.port = ntohs(addr->sin6_port);
        *endpoint = out;
        return true;
    }
    return false;
}

C_TcpSocketResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    return galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid)
        ? C_TcpSocketParameterInvalid
        : C_TcpSocketIOFailed;
}

bool timeout_fits_chrono(int64_t timeout_ms)
{
    if (timeout_ms <= 0) {
        return true;
    }
    using MillisecondsRep = std::chrono::milliseconds::rep;
    using NanosecondsRep = std::chrono::nanoseconds::rep;
    constexpr auto max_milliseconds_rep =
        static_cast<int64_t>(std::numeric_limits<MillisecondsRep>::max());
    constexpr auto max_milliseconds_for_nanoseconds =
        static_cast<int64_t>(std::numeric_limits<NanosecondsRep>::max() / 1000000);
    constexpr int64_t max_supported_milliseconds =
        max_milliseconds_rep < max_milliseconds_for_nanoseconds
            ? max_milliseconds_rep
            : max_milliseconds_for_nanoseconds;
    return timeout_ms <= max_supported_milliseconds;
}

GalayCoreCoroIOResultCode to_core_code(C_IOResultCode code)
{
    switch (code) {
    case C_IOResultOk:
        return GalayCoreCoroIOResultOk;
    case C_IOResultEof:
        return GalayCoreCoroIOResultEof;
    case C_IOResultTimeout:
        return GalayCoreCoroIOResultTimeout;
    case C_IOResultCancelled:
        return GalayCoreCoroIOResultCancelled;
    case C_IOResultInvalid:
        return GalayCoreCoroIOResultInvalid;
    case C_IOResultError:
        return GalayCoreCoroIOResultError;
    }
    return GalayCoreCoroIOResultError;
}

C_IOResultCode from_core_code(GalayCoreCoroIOResultCode code)
{
    switch (code) {
    case GalayCoreCoroIOResultOk:
        return C_IOResultOk;
    case GalayCoreCoroIOResultEof:
        return C_IOResultEof;
    case GalayCoreCoroIOResultTimeout:
        return C_IOResultTimeout;
    case GalayCoreCoroIOResultCancelled:
        return C_IOResultCancelled;
    case GalayCoreCoroIOResultInvalid:
        return C_IOResultInvalid;
    case GalayCoreCoroIOResultError:
        return C_IOResultError;
    }
    return C_IOResultError;
}

GalayCoreCoroIOResult to_core_result(C_IOResult result)
{
    return GalayCoreCoroIOResult{
        to_core_code(result.code),
        result.sys_errno,
        result.bytes,
        result.value,
        result.ptr,
    };
}

C_IOResult from_core_result(GalayCoreCoroIOResult result)
{
    return C_IOResult{
        from_core_code(result.code),
        result.sys_errno,
        result.bytes,
        result.value,
        result.ptr,
    };
}

GalayCoreCoroHost to_core_host(const C_Host& host)
{
    GalayCoreCoroHost out{};
    out.type = host.type == C_IPTypeIPV4
        ? GalayCoreCoroIPTypeIPV4
        : GalayCoreCoroIPTypeIPV6;
    std::memcpy(out.address, host.address, sizeof(out.address));
    out.address[sizeof(out.address) - 1] = '\0';
    out.port = host.port;
    return out;
}

GalayCoreIOScheduler* current_io_scheduler()
{
    return reinterpret_cast<GalayCoreIOScheduler*>(
        galay::kernel::coro_c::currentTaskOwnerScheduler());
}

GalayCoreTcpSocket* to_core_socket(void* socket)
{
    return reinterpret_cast<GalayCoreTcpSocket*>(socket);
}

galay::async::TcpSocket* to_cpp_socket(GalayCoreTcpSocket* socket)
{
    return reinterpret_cast<galay::async::TcpSocket*>(socket);
}

struct WaitRequestScope {
    C_CoroWaitRequest request{nullptr};
    void* user_data = nullptr;

    ~WaitRequestScope()
    {
        if (request.request != nullptr) {
            C_IOResult cleanup_result = galay_coro_wait_request_destroy(&request);
            request.request = cleanup_result.code == C_IOResultOk ? nullptr : request.request;
        }
    }
};

C_IOResult merge_cleanup_result(C_IOResult primary, C_IOResult cleanup)
{
    return primary.code == C_IOResultOk && cleanup.code != C_IOResultOk
        ? cleanup
        : primary;
}

C_IOResult close_wait_scope(WaitRequestScope& scope)
{
    if (scope.request.request == nullptr) {
        return make_result(C_IOResultOk);
    }
    return galay_coro_wait_request_destroy(&scope.request);
}

GalayCoreCoroIOResult wait_request(void* ctx, int64_t timeout_ms)
{
    auto* scope = static_cast<WaitRequestScope*>(ctx);
    if (scope == nullptr) {
        return to_core_result(make_result(C_IOResultInvalid));
    }
    return to_core_result(galay_coro_wait(&scope->request, timeout_ms));
}

GalayCoreCoroIOResult complete_user_data(void* user_data,
                                         GalayCoreCoroIOResult result)
{
    return to_core_result(
        galay_coro_wait_event_user_data_complete(user_data, from_core_result(result)));
}

GalayCoreCoroIOResult release_user_data(void* user_data)
{
    return to_core_result(galay_coro_wait_event_user_data_release(user_data));
}

C_IOResult prepare_wait_user_data(WaitRequestScope& scope)
{
    C_IOResult created = galay_coro_wait_request_create(&scope.request);
    if (created.code != C_IOResultOk) {
        return created;
    }

    uint64_t generation = 0;
    C_IOResult prepared = galay_coro_wait_request_prepare(&scope.request, &generation);
    if (prepared.code != C_IOResultOk) {
        return prepared;
    }

    C_CoroWaitEventToken token{nullptr};
    C_IOResult acquired =
        galay_coro_wait_request_event_token_acquire(&scope.request, generation, &token);
    if (acquired.code != C_IOResultOk) {
        C_IOResult cancelled = galay_coro_wait_request_cancel(&scope.request, generation);
        return merge_cleanup_result(acquired, cancelled);
    }

    C_IOResult detached =
        galay_coro_wait_event_token_detach_user_data(&token, &scope.user_data);
    if (detached.code != C_IOResultOk) {
        C_IOResult released = galay_coro_wait_event_token_release(&token);
        detached = merge_cleanup_result(detached, released);
        C_IOResult cancelled = galay_coro_wait_request_cancel(&scope.request, generation);
        detached = merge_cleanup_result(detached, cancelled);
    }
    return detached;
}

GalayCoreCoroWaitOps make_wait_ops(WaitRequestScope& scope)
{
    return GalayCoreCoroWaitOps{
        wait_request,
        complete_user_data,
        release_user_data,
        &scope,
    };
}

template <typename Submit>
C_IOResult submit_with_wait(Submit&& submit)
{
    WaitRequestScope scope;
    C_IOResult prepared = prepare_wait_user_data(scope);
    if (prepared.code != C_IOResultOk) {
        return prepared;
    }

    GalayCoreCoroWaitOps wait_ops = make_wait_ops(scope);
    C_IOResult result = from_core_result(std::forward<Submit>(submit)(scope.user_data, &wait_ops));
    return merge_cleanup_result(result, close_wait_scope(scope));
}

} // namespace

extern "C" {

const char* galay_kernel_tcp_socket_get_error(C_TcpSocketResultCode code)
{
    switch (code) {
    case C_TcpSocketSuccess:
        return "success";
    case C_TcpSocketParameterInvalid:
        return "parameter invalid";
    case C_TcpSocketMemoryAllocFailed:
        return "memory allocation failed";
    case C_TcpSocketIOFailed:
        return "io failed";
    case C_TcpSocketOperationInvalid:
        return "operation invalid";
    }
    return "unknown tcp socket error";
}

C_TcpSocketResultCode galay_kernel_tcp_socket_create(
    galay_kernel_tcp_socket_t* c_socket,
    C_IPType type)
{
    if (c_socket == nullptr || !is_valid_c_ip_type(type)) {
        return C_TcpSocketParameterInvalid;
    }

    c_socket->socket = nullptr;
    auto result = galay::async::TcpSocket::create(from_c_ip_type_to_cpp_ip_type(type));
    if (!result) {
        return C_TcpSocketMemoryAllocFailed;
    }

    auto* socket = new (std::nothrow) galay::async::TcpSocket(std::move(*result));
    if (socket == nullptr) {
        return C_TcpSocketMemoryAllocFailed;
    }

    c_socket->socket = socket;
    return C_TcpSocketSuccess;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t* c_socket)
{
    if (c_socket == nullptr) {
        return C_TcpSocketParameterInvalid;
    }
    delete static_cast<galay::async::TcpSocket*>(c_socket->socket);
    c_socket->socket = nullptr;
    return C_TcpSocketSuccess;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_bind(
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host)
{
    if (c_socket == nullptr || c_socket->socket == nullptr ||
        host == nullptr || !is_valid_c_ip_type(host->type)) {
        return C_TcpSocketParameterInvalid;
    }

    auto cpp_host = from_c_host_to_cpp_host(*host);
    if (!cpp_host.valid()) {
        return C_TcpSocketParameterInvalid;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto reuse_addr = socket->option().handleReuseAddr();
    if (!reuse_addr) {
        return from_cpp_io_error(reuse_addr.error());
    }
    auto non_block = socket->option().handleNonBlock();
    if (!non_block) {
        return from_cpp_io_error(non_block.error());
    }
    auto bound = socket->bind(cpp_host);
    return bound ? C_TcpSocketSuccess : from_cpp_io_error(bound.error());
}

C_TcpSocketResultCode galay_kernel_tcp_socket_listen(
    galay_kernel_tcp_socket_t* c_socket,
    int backlog)
{
    if (c_socket == nullptr || c_socket->socket == nullptr) {
        return C_TcpSocketParameterInvalid;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto listened = socket->listen(backlog);
    return listened ? C_TcpSocketSuccess : from_cpp_io_error(listened.error());
}

C_TcpSocketResultCode galay_kernel_tcp_socket_local_endpoint(
    const galay_kernel_tcp_socket_t* c_socket,
    C_Host* endpoint)
{
    if (c_socket == nullptr || c_socket->socket == nullptr || endpoint == nullptr) {
        return C_TcpSocketParameterInvalid;
    }

    const auto* socket = static_cast<const galay::async::TcpSocket*>(c_socket->socket);
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(socket->handle().fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return C_TcpSocketIOFailed;
    }

    return assign_c_host_from_sockaddr(storage, endpoint)
        ? C_TcpSocketSuccess
        : C_TcpSocketIOFailed;
}

C_IOResult galay_kernel_tcp_socket_accept(
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_socket_t* out_socket,
    C_Host* out_peer,
    int64_t timeout_ms)
{
    GalayCoreIOScheduler* scheduler = current_io_scheduler();
    if (listener == nullptr || listener->socket == nullptr ||
        out_socket == nullptr || out_socket->socket != nullptr ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    GalayCoreTcpSocket* accepted_socket = nullptr;
    C_IOResult result = submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_tcp_accept(to_core_socket(listener->socket),
                                              scheduler,
                                              &accepted_socket,
                                              reinterpret_cast<GalayCoreCoroHost*>(out_peer),
                                              timeout_ms,
                                              user_data,
                                              wait_ops);
        });
    if (result.code != C_IOResultOk) {
        delete to_cpp_socket(accepted_socket);
        return result;
    }
    out_socket->socket = accepted_socket;
    result.ptr = out_socket;
    return result;
}

C_IOResult galay_kernel_tcp_socket_connect(
    galay_kernel_tcp_socket_t* socket,
    const C_Host* host,
    int64_t timeout_ms)
{
    GalayCoreIOScheduler* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        host == nullptr || !is_valid_c_ip_type(host->type) ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    GalayCoreCoroHost core_host = to_core_host(*host);
    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_tcp_connect(to_core_socket(socket->socket),
                                               scheduler,
                                               &core_host,
                                               timeout_ms,
                                               user_data,
                                               wait_ops);
        });
}

C_IOResult galay_kernel_tcp_socket_recv(
    galay_kernel_tcp_socket_t* socket,
    char* buffer,
    size_t length,
    int64_t timeout_ms)
{
    GalayCoreIOScheduler* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        buffer == nullptr || length == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_tcp_recv(to_core_socket(socket->socket),
                                            scheduler,
                                            buffer,
                                            length,
                                            timeout_ms,
                                            user_data,
                                            wait_ops);
        });
}

C_IOResult galay_kernel_tcp_socket_send(
    galay_kernel_tcp_socket_t* socket,
    const char* buffer,
    size_t length,
    int64_t timeout_ms)
{
    GalayCoreIOScheduler* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        buffer == nullptr || length == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_tcp_send(to_core_socket(socket->socket),
                                            scheduler,
                                            buffer,
                                            length,
                                            timeout_ms,
                                            user_data,
                                            wait_ops);
        });
}

C_IOResult galay_kernel_tcp_socket_readv(
    galay_kernel_tcp_socket_t* socket,
    const galay_iovec_t* iovecs,
    size_t count,
    int64_t timeout_ms)
{
    GalayCoreIOScheduler* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        iovecs == nullptr || count == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_tcp_readv(to_core_socket(socket->socket),
                                             scheduler,
                                             iovecs,
                                             count,
                                             timeout_ms,
                                             user_data,
                                             wait_ops);
        });
}

C_IOResult galay_kernel_tcp_socket_writev(
    galay_kernel_tcp_socket_t* socket,
    const galay_iovec_t* iovecs,
    size_t count,
    int64_t timeout_ms)
{
    GalayCoreIOScheduler* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        iovecs == nullptr || count == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_tcp_writev(to_core_socket(socket->socket),
                                              scheduler,
                                              iovecs,
                                              count,
                                              timeout_ms,
                                              user_data,
                                              wait_ops);
        });
}

C_IOResult galay_kernel_tcp_socket_sendfile(
    galay_kernel_tcp_socket_t* socket,
    int file_fd,
    int64_t offset,
    size_t count,
    int64_t timeout_ms)
{
    GalayCoreIOScheduler* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        file_fd < 0 || offset < 0 || count == 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_tcp_sendfile(to_core_socket(socket->socket),
                                                scheduler,
                                                file_fd,
                                                offset,
                                                count,
                                                timeout_ms,
                                                user_data,
                                                wait_ops);
        });
}

C_IOResult galay_kernel_tcp_socket_close(
    galay_kernel_tcp_socket_t* socket,
    int64_t timeout_ms)
{
    GalayCoreIOScheduler* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    return from_core_result(
        galay_core_coro_tcp_close(to_core_socket(socket->socket), scheduler, timeout_ms));
}

} // extern "C"
