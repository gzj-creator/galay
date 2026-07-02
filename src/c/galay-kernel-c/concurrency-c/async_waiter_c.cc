#include "async_waiter_c.h"

#include "../../../cpp/galay-kernel/concurrency/async_waiter.h"
#include "../coro-c/coro_task_internal.hpp"
#include "../coro-c/coro_wait_c.h"
#include <galay/c/galay-bridge-c/coro-c/c_coro_async_waiter_bridge.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace
{

galay::kernel::AsyncWaiter<void>* to_cpp_waiter(galay_kernel_async_waiter_t* waiter)
{
    return static_cast<galay::kernel::AsyncWaiter<void>*>(waiter->waiter);
}

const galay::kernel::AsyncWaiter<void>* to_cpp_waiter(const galay_kernel_async_waiter_t* waiter)
{
    return static_cast<const galay::kernel::AsyncWaiter<void>*>(waiter->waiter);
}

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
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

GalayCoreIOScheduler* current_scheduler()
{
    return reinterpret_cast<GalayCoreIOScheduler*>(
        galay::kernel::coro_c::currentTaskOwnerScheduler());
}

GalayCoreAsyncWaiter* to_core_waiter(void* waiter)
{
    return reinterpret_cast<GalayCoreAsyncWaiter*>(waiter);
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

const char* galay_kernel_async_waiter_get_error(C_AsyncWaiterResultCode code)
{
    switch (code)
    {
    case C_AsyncWaiterSuccess:
        return "success";
    case C_AsyncWaiterParameterInvalid:
        return "parameter invalid";
    case C_AsyncWaiterMemoryAllocFailed:
        return "memory allocation failed";
    case C_AsyncWaiterIOFailed:
        return "io failed";
    case C_AsyncWaiterOperationInvalid:
        return "operation invalid";
    case C_AsyncWaiterTimeout:
        return "timeout";
    }
    return "unknown async waiter error";
}

C_AsyncWaiterResultCode galay_kernel_async_waiter_create(galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr)
    {
        return C_AsyncWaiterParameterInvalid;
    }

    c_waiter->waiter = nullptr;
    auto* waiter = new (std::nothrow) galay::kernel::AsyncWaiter<void>();
    if (waiter == nullptr)
    {
        return C_AsyncWaiterMemoryAllocFailed;
    }

    c_waiter->waiter = waiter;
    return C_AsyncWaiterSuccess;
}

C_AsyncWaiterResultCode galay_kernel_async_waiter_destroy(galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr)
    {
        return C_AsyncWaiterParameterInvalid;
    }

    delete to_cpp_waiter(c_waiter);
    c_waiter->waiter = nullptr;
    return C_AsyncWaiterSuccess;
}

bool galay_kernel_async_waiter_is_waiting(const galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr || c_waiter->waiter == nullptr)
    {
        return false;
    }
    return to_cpp_waiter(c_waiter)->isWaiting();
}

bool galay_kernel_async_waiter_is_ready(const galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr || c_waiter->waiter == nullptr)
    {
        return false;
    }
    return to_cpp_waiter(c_waiter)->isReady();
}

C_AsyncWaiterResultCode galay_kernel_async_waiter_notify(galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr || c_waiter->waiter == nullptr)
    {
        return C_AsyncWaiterParameterInvalid;
    }

    return to_cpp_waiter(c_waiter)->notify()
        ? C_AsyncWaiterSuccess
        : C_AsyncWaiterOperationInvalid;
}

C_IOResult galay_kernel_async_waiter_wait(
    galay_kernel_async_waiter_t* c_waiter,
    int64_t timeout_ms)
{
    GalayCoreIOScheduler* scheduler = current_scheduler();
    if (c_waiter == nullptr || c_waiter->waiter == nullptr ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_async_waiter_wait(to_core_waiter(c_waiter->waiter),
                                                     scheduler,
                                                     timeout_ms,
                                                     user_data,
                                                     wait_ops);
        });
}
