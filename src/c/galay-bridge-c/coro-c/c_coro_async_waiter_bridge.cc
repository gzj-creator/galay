#include "c_coro_async_waiter_bridge.h"

#include <galay/cpp/galay-kernel/concurrency/async_waiter.h>
#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <utility>

namespace
{

using C_IOResult = GalayCoreCoroIOResult;
using C_IOResultCode = GalayCoreCoroIOResultCode;
using galay::kernel::AsyncWaiter;
using galay::kernel::AsyncWaiterAwaitable;
using galay::kernel::IOError;
using galay::kernel::Scheduler;

constexpr C_IOResultCode C_IOResultOk = GalayCoreCoroIOResultOk;
constexpr C_IOResultCode C_IOResultTimeout = GalayCoreCoroIOResultTimeout;
constexpr C_IOResultCode C_IOResultInvalid = GalayCoreCoroIOResultInvalid;
constexpr C_IOResultCode C_IOResultError = GalayCoreCoroIOResultError;

struct CoroAsyncWaiterOperation;

struct CoroAsyncWaiterWakeState {
    galay::kernel::detail::ResumeTokenHeader header;
    CoroAsyncWaiterOperation* operation = nullptr;
};

enum class CompletionPhase : uint8_t {
    Pending,
    TimedOut,
    Completed,
};

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

C_IOResult merge_cleanup_result(C_IOResult primary, C_IOResult cleanup)
{
    return primary.code == C_IOResultOk && cleanup.code != C_IOResultOk
        ? cleanup
        : primary;
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

bool valid_wait_ops(const GalayCoreCoroWaitOps* wait_ops)
{
    return wait_ops != nullptr &&
        wait_ops->wait != nullptr &&
        wait_ops->complete_user_data != nullptr &&
        wait_ops->release_user_data != nullptr;
}

C_IOResult from_io_error(const IOError& error)
{
    const int sys_errno = static_cast<int>(error.code() >> 32U);
    if (IOError::contains(error.code(), galay::kernel::kTimeout)) {
        return make_result(C_IOResultTimeout, sys_errno);
    }
    if (IOError::contains(error.code(), galay::kernel::kParamInvalid)) {
        return make_result(C_IOResultInvalid, sys_errno);
    }
    return make_result(C_IOResultError, sys_errno);
}

AsyncWaiter<void>* to_cpp_waiter(GalayCoreAsyncWaiter* waiter)
{
    return reinterpret_cast<AsyncWaiter<void>*>(waiter);
}

Scheduler* to_scheduler(GalayCoreIOScheduler* scheduler_handle)
{
    return reinterpret_cast<Scheduler*>(scheduler_handle);
}

struct CoroAsyncWaiterOperation {
    CoroAsyncWaiterOperation(AsyncWaiter<void>* waiter,
                             Scheduler* scheduler,
                             void* user_data,
                             GalayCoreCoroWaitOps wait_ops)
        : awaitable(waiter->wait())
        , m_wait_ops(wait_ops)
        , m_scheduler(scheduler)
    {
        m_user_data = user_data;
        m_wake_state.header.hooks = &kWakeHooks;
        m_wake_state.operation = this;
    }

    galay::kernel::Waker makeWaker() noexcept
    {
        return galay::kernel::Waker(
            galay::kernel::detail::ResumeToken::fromCCoroutine(&m_wake_state));
    }

    bool completeFromWake() noexcept
    {
        CompletionPhase expected = CompletionPhase::Pending;
        if (!m_phase.compare_exchange_strong(expected,
                                             CompletionPhase::Completed,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return true;
        }
        return completeUserData(buildResult());
    }

    C_IOResult wait(int64_t timeout_ms) noexcept
    {
        return m_wait_ops.wait != nullptr
            ? m_wait_ops.wait(m_wait_ops.ctx, timeout_ms)
            : make_result(C_IOResultInvalid);
    }

    void markWaitTimeout() noexcept
    {
        CompletionPhase expected = CompletionPhase::Pending;
        if (m_phase.compare_exchange_strong(expected,
                                            CompletionPhase::TimedOut,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            awaitable.markTimeout();
        }
    }

    bool hasPendingToken() noexcept
    {
        std::lock_guard<std::mutex> lock(m_user_data_mutex);
        return m_user_data != nullptr;
    }

    C_IOResult finishWithoutWait(C_IOResult result) noexcept
    {
        C_IOResult completed = completeAndReleaseUserData(result);
        return merge_cleanup_result(result, completed);
    }

    Scheduler* scheduler() const noexcept { return m_scheduler; }

    AsyncWaiterAwaitable<void> awaitable;

private:
    C_IOResult buildResult() noexcept
    {
        auto resumed = awaitable.await_resume();
        return resumed ? make_result(C_IOResultOk) : from_io_error(resumed.error());
    }

    bool completeUserData(C_IOResult result) noexcept
    {
        const C_IOResult completed = completeAndReleaseUserData(result);
        return completed.code == C_IOResultOk || completed.code == C_IOResultInvalid;
    }

    C_IOResult completeAndReleaseUserData(C_IOResult result) noexcept
    {
        void* user_data = nullptr;
        auto complete_user_data = m_wait_ops.complete_user_data;
        auto release_user_data = m_wait_ops.release_user_data;
        C_IOResult completed = make_result(C_IOResultInvalid);
        {
            std::lock_guard<std::mutex> lock(m_user_data_mutex);
            user_data = std::exchange(m_user_data, nullptr);
            if (user_data != nullptr && complete_user_data != nullptr) {
                completed = complete_user_data(user_data, result);
            }
        }
        if (user_data != nullptr && release_user_data != nullptr) {
            C_IOResult released = release_user_data(user_data);
            completed = merge_cleanup_result(completed, released);
        }
        return completed;
    }

    static Scheduler* wake_owner(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroAsyncWaiterWakeState*>(state);
        return wake_state != nullptr && wake_state->operation != nullptr
            ? wake_state->operation->scheduler()
            : nullptr;
    }

    static bool wake_request(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroAsyncWaiterWakeState*>(state);
        return wake_state != nullptr && wake_state->operation != nullptr &&
            wake_state->operation->completeFromWake();
    }

    static void wake_retain(void*) noexcept {}
    static void wake_release(void*) noexcept {}

    inline static const galay::kernel::detail::ResumeTokenHooks kWakeHooks{
        .owner_scheduler = wake_owner,
        .request_resume = wake_request,
        .retain = wake_retain,
        .release = wake_release,
    };

    std::mutex m_user_data_mutex;
    CoroAsyncWaiterWakeState m_wake_state{};
    GalayCoreCoroWaitOps m_wait_ops{};
    Scheduler* m_scheduler = nullptr;
    void* m_user_data = nullptr;
    std::atomic<CompletionPhase> m_phase{CompletionPhase::Pending};
};

} // namespace

extern "C" {

GalayCoreCoroIOResult galay_core_coro_async_waiter_wait(
    GalayCoreAsyncWaiter* waiter_handle,
    GalayCoreIOScheduler* scheduler_handle,
    int64_t timeout_ms,
    void* user_data,
    const GalayCoreCoroWaitOps* wait_ops)
{
    auto* waiter = to_cpp_waiter(waiter_handle);
    auto* scheduler = to_scheduler(scheduler_handle);
    if (waiter == nullptr || scheduler == nullptr || user_data == nullptr ||
        !timeout_fits_chrono(timeout_ms) || !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    CoroAsyncWaiterOperation operation(waiter, scheduler, user_data, *wait_ops);
    if (operation.awaitable.await_ready()) {
        return operation.finishWithoutWait(make_result(C_IOResultOk));
    }
    const bool suspended = operation.awaitable.await_suspend(operation.makeWaker());
    if (!suspended) {
        auto resumed = operation.awaitable.await_resume();
        C_IOResult immediate_result = resumed
            ? make_result(C_IOResultOk)
            : from_io_error(resumed.error());
        return operation.finishWithoutWait(immediate_result);
    }

    C_IOResult result = operation.wait(timeout_ms);
    if (result.code == C_IOResultTimeout) {
        operation.markWaitTimeout();
    }
    if (operation.hasPendingToken()) {
        result = operation.finishWithoutWait(result);
    }
    return result;
}

} // extern "C"
