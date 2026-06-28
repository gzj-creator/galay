#include "tcp_socket_coro_c.h"

#include "../coro-c/coro_task_internal.hpp"
#include "../coro-c/coro_wait_c.h"
#include <galay/cpp/galay-kernel/core/c_coro_tcp_bridge.h>

#include <chrono>
#include <cstring>
#include <limits>
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
            (void)galay_coro_wait_request_destroy(&request);
        }
    }
};

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
        (void)galay_coro_wait_request_cancel(&scope.request, generation);
        return acquired;
    }

    C_IOResult detached =
        galay_coro_wait_event_token_detach_user_data(&token, &scope.user_data);
    if (detached.code != C_IOResultOk) {
        (void)galay_coro_wait_event_token_release(&token);
        (void)galay_coro_wait_request_cancel(&scope.request, generation);
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
    return from_core_result(std::forward<Submit>(submit)(scope.user_data, &wait_ops));
}

} // namespace

extern "C" {

C_IOResult galay_coro_tcp_accept(galay_kernel_tcp_socket_t* listener,
                                 galay_kernel_tcp_socket_t* out_socket,
                                 int64_t timeout_ms)
{
    try {
        void* scheduler = current_io_scheduler();
        if (listener == nullptr || listener->socket == nullptr ||
            out_socket == nullptr || out_socket->socket != nullptr ||
            scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
            return make_result(C_IOResultInvalid);
        }
        if (timeout_ms == 0) {
            return make_result(C_IOResultTimeout);
        }

        C_IOResult result = submit_with_wait(
            [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
                return galay_core_coro_tcp_accept(listener->socket,
                                                  scheduler,
                                                  &out_socket->socket,
                                                  timeout_ms,
                                                  user_data,
                                                  wait_ops);
            });
        if (result.code == C_IOResultOk) {
            result.ptr = out_socket;
        }
        return result;
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_tcp_connect(galay_kernel_tcp_socket_t* socket,
                                  const C_Host* host,
                                  int64_t timeout_ms)
{
    try {
        void* scheduler = current_io_scheduler();
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
                return galay_core_coro_tcp_connect(socket->socket,
                                                   scheduler,
                                                   &core_host,
                                                   timeout_ms,
                                                   user_data,
                                                   wait_ops);
            });
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_tcp_recv(galay_kernel_tcp_socket_t* socket,
                               char* buffer,
                               size_t length,
                               int64_t timeout_ms)
{
    try {
        void* scheduler = current_io_scheduler();
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
                return galay_core_coro_tcp_recv(socket->socket,
                                                scheduler,
                                                buffer,
                                                length,
                                                timeout_ms,
                                                user_data,
                                                wait_ops);
            });
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_tcp_send(galay_kernel_tcp_socket_t* socket,
                               const char* buffer,
                               size_t length,
                               int64_t timeout_ms)
{
    try {
        void* scheduler = current_io_scheduler();
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
                return galay_core_coro_tcp_send(socket->socket,
                                                scheduler,
                                                buffer,
                                                length,
                                                timeout_ms,
                                                user_data,
                                                wait_ops);
            });
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_tcp_close(galay_kernel_tcp_socket_t* socket,
                                int64_t timeout_ms)
{
    try {
        void* scheduler = current_io_scheduler();
        if (socket == nullptr || socket->socket == nullptr || scheduler == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        return from_core_result(
            galay_core_coro_tcp_close(socket->socket, scheduler, timeout_ms));
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

} // extern "C"
