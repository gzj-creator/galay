#include "udp_socket_c.h"

#include "../../../cpp/galay-kernel/async/udp_socket.h"
#include "../coro-c/coro_task_internal.hpp"
#include "../coro-c/coro_wait_c.h"
#include <galay/c/galay-bridge-c/coro-c/c_coro_udp_bridge.h>

#include <arpa/inet.h>
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

C_UdpSocketResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    return galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid)
        ? C_UdpSocketParameterInvalid
        : C_UdpSocketIOFailed;
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

void* current_io_scheduler()
{
    return static_cast<void*>(galay::kernel::coro_c::currentTaskOwnerScheduler());
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

const char* galay_kernel_udp_socket_get_error(C_UdpSocketResultCode code)
{
    switch (code) {
    case C_UdpSocketSuccess:
        return "success";
    case C_UdpSocketParameterInvalid:
        return "parameter invalid";
    case C_UdpSocketMemoryAllocFailed:
        return "memory allocation failed";
    case C_UdpSocketIOFailed:
        return "io failed";
    case C_UdpSocketOperationInvalid:
        return "operation invalid";
    }
    return "unknown udp socket error";
}

C_UdpSocketResultCode galay_kernel_udp_socket_create(
    galay_kernel_udp_socket_t* c_socket,
    C_IPType type)
{
    if (c_socket == nullptr || !is_valid_c_ip_type(type)) {
        return C_UdpSocketParameterInvalid;
    }

    c_socket->socket = nullptr;
    auto result = galay::async::UdpSocket::create(from_c_ip_type_to_cpp_ip_type(type));
    if (!result) {
        return from_cpp_io_error(result.error());
    }

    auto* socket = new (std::nothrow) galay::async::UdpSocket(std::move(*result));
    if (socket == nullptr) {
        return C_UdpSocketMemoryAllocFailed;
    }

    c_socket->socket = socket;
    return C_UdpSocketSuccess;
}

C_UdpSocketResultCode galay_kernel_udp_socket_destroy(galay_kernel_udp_socket_t* c_socket)
{
    if (c_socket == nullptr) {
        return C_UdpSocketParameterInvalid;
    }
    delete static_cast<galay::async::UdpSocket*>(c_socket->socket);
    c_socket->socket = nullptr;
    return C_UdpSocketSuccess;
}

C_UdpSocketResultCode galay_kernel_udp_socket_bind(
    galay_kernel_udp_socket_t* c_socket,
    const C_Host* host)
{
    if (c_socket == nullptr || c_socket->socket == nullptr ||
        host == nullptr || !is_valid_c_ip_type(host->type)) {
        return C_UdpSocketParameterInvalid;
    }

    auto cpp_host = from_c_host_to_cpp_host(*host);
    if (!cpp_host.valid()) {
        return C_UdpSocketParameterInvalid;
    }

    auto* socket = static_cast<galay::async::UdpSocket*>(c_socket->socket);
    auto reuse_addr = socket->option().handleReuseAddr();
    if (!reuse_addr) {
        return C_UdpSocketIOFailed;
    }
    auto non_block = socket->option().handleNonBlock();
    if (!non_block) {
        return C_UdpSocketIOFailed;
    }
    auto bound = socket->bind(cpp_host);
    return bound ? C_UdpSocketSuccess : from_cpp_io_error(bound.error());
}

C_UdpSocketResultCode galay_kernel_udp_socket_local_endpoint(
    const galay_kernel_udp_socket_t* c_socket,
    C_Host* endpoint)
{
    if (c_socket == nullptr || c_socket->socket == nullptr || endpoint == nullptr) {
        return C_UdpSocketParameterInvalid;
    }

    const auto* socket = static_cast<const galay::async::UdpSocket*>(c_socket->socket);
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(socket->handle().fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return C_UdpSocketIOFailed;
    }

    return assign_c_host_from_sockaddr(storage, endpoint)
        ? C_UdpSocketSuccess
        : C_UdpSocketIOFailed;
}

C_IOResult galay_kernel_udp_socket_recvfrom(
    galay_kernel_udp_socket_t* socket,
    char* buffer,
    size_t length,
    C_Host* from,
    int64_t timeout_ms)
{
    void* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        (buffer == nullptr && length != 0) ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_udp_recvfrom(socket->socket,
                                                scheduler,
                                                buffer,
                                                length,
                                                reinterpret_cast<GalayCoreCoroHost*>(from),
                                                timeout_ms,
                                                user_data,
                                                wait_ops);
        });
}

C_IOResult galay_kernel_udp_socket_sendto(
    galay_kernel_udp_socket_t* socket,
    const char* buffer,
    size_t length,
    const C_Host* to,
    int64_t timeout_ms)
{
    void* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        (buffer == nullptr && length != 0) ||
        to == nullptr || !is_valid_c_ip_type(to->type) ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    auto cpp_to = from_c_host_to_cpp_host(*to);
    if (!cpp_to.valid()) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    GalayCoreCoroHost core_to = to_core_host(*to);
    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_udp_sendto(socket->socket,
                                              scheduler,
                                              buffer,
                                              length,
                                              &core_to,
                                              timeout_ms,
                                              user_data,
                                              wait_ops);
        });
}

C_IOResult galay_kernel_udp_socket_close(
    galay_kernel_udp_socket_t* socket,
    int64_t timeout_ms)
{
    void* scheduler = current_io_scheduler();
    if (socket == nullptr || socket->socket == nullptr ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    return from_core_result(
        galay_core_coro_udp_close(socket->socket, scheduler, timeout_ms));
}

} // extern "C"
