#include "c_coro_aio_file_bridge.h"

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/async/aio_file.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <utility>
#endif

#include <cerrno>

namespace
{

using C_IOResult = GalayCoreCoroIOResult;
using C_IOResultCode = GalayCoreCoroIOResultCode;

constexpr C_IOResultCode C_IOResultOk = GalayCoreCoroIOResultOk;
constexpr C_IOResultCode C_IOResultTimeout = GalayCoreCoroIOResultTimeout;
constexpr C_IOResultCode C_IOResultInvalid = GalayCoreCoroIOResultInvalid;
constexpr C_IOResultCode C_IOResultError = GalayCoreCoroIOResultError;

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

#ifdef USE_EPOLL

using galay::async::AioCommitAwaitable;
using galay::async::AioFile;
using galay::kernel::IOController;
using galay::kernel::IOError;
using galay::kernel::IOScheduler;
using galay::kernel::Scheduler;

int io_error_sys_errno(const IOError& error)
{
    return static_cast<int>(error.code() >> 32U);
}

C_IOResult from_io_error(const IOError& error)
{
    if (IOError::contains(error.code(), galay::kernel::kParamInvalid) ||
        IOError::contains(error.code(), galay::kernel::kNotRunningOnIOScheduler) ||
        IOError::contains(error.code(), galay::kernel::kNotReady)) {
        return make_result(C_IOResultInvalid, io_error_sys_errno(error));
    }
    if (IOError::contains(error.code(), galay::kernel::kTimeout)) {
        return make_result(C_IOResultTimeout, io_error_sys_errno(error));
    }
    return make_result(C_IOResultError, io_error_sys_errno(error));
}

AioFile* to_cpp_file(void* file)
{
    return static_cast<AioFile*>(file);
}

Scheduler* to_io_scheduler(void* scheduler_handle)
{
    auto* scheduler = static_cast<Scheduler*>(scheduler_handle);
    return scheduler != nullptr && scheduler->type() == galay::kernel::kIOScheduler
        ? scheduler
        : nullptr;
}

bool valid_wait_ops(const GalayCoreCoroWaitOps* wait_ops)
{
    return wait_ops != nullptr &&
        wait_ops->wait != nullptr &&
        wait_ops->complete_user_data != nullptr &&
        wait_ops->release_user_data != nullptr;
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

struct CoroAioCommitOperation;

struct CoroAioCommitWakeState {
    galay::kernel::detail::ResumeTokenHeader header;
    CoroAioCommitOperation* operation = nullptr;
};

enum class CoroAioCommitPhase : uint8_t {
    Pending,
    TimedOut,
    Completed,
};

struct CoroAioCommitOperation {
    CoroAioCommitOperation(AioCommitAwaitable&& awaitable,
                           Scheduler* scheduler,
                           void* user_data,
                           GalayCoreCoroWaitOps wait_ops,
                           ssize_t* results,
                           size_t result_capacity,
                           size_t* out_count)
        : m_awaitable(std::move(awaitable))
        , m_scheduler(scheduler)
        , m_wait_ops(wait_ops)
        , m_user_data(user_data)
        , m_results(results)
        , m_result_capacity(result_capacity)
        , m_out_count(out_count)
    {
        m_wake_state.header.hooks = &kWakeHooks;
        m_wake_state.operation = this;
        m_awaitable.m_waker = makeWaker();
    }

    galay::kernel::Waker makeWaker() noexcept
    {
        return galay::kernel::Waker(
            galay::kernel::detail::ResumeToken::fromCCoroutine(&m_wake_state));
    }

    C_IOResult submitAndWait(int64_t timeout_ms)
    {
        if (m_awaitable.await_ready()) {
            return finishWithoutWait(buildResult());
        }

        IOController* controller = m_awaitable.m_controller;
        if (controller == nullptr || controller->m_awaitable[IOController::READ] != nullptr ||
            controller->m_sequence_owner[IOController::READ] != nullptr) {
            return finishWithoutWait(make_result(C_IOResultInvalid));
        }

        const int submitted = io_submit(m_awaitable.m_aio_ctx,
                                        m_awaitable.m_pending_count,
                                        m_awaitable.m_pending_ptrs.data());
        if (submitted < 0) {
            m_awaitable.m_result = std::unexpected(
                galay::kernel::IOError(galay::kernel::kWriteFailed,
                                       static_cast<uint32_t>(-submitted)));
            return finishWithoutWait(buildResult());
        }

        if (!controller->fillAwaitable(FILEREAD, &m_awaitable)) {
            return finishWithoutWait(make_result(C_IOResultInvalid));
        }

        controller->m_handle.fd = m_awaitable.m_event_fd;
        const int registered = static_cast<IOScheduler*>(m_scheduler)->addFileRead(controller);
        if (registered == 1) {
            controller->removeAwaitable(FILEREAD);
            return finishWithoutWait(buildResult());
        }
        if (registered < 0) {
            controller->removeAwaitable(FILEREAD);
            return finishWithoutWait(
                make_result(C_IOResultError,
                            galay::kernel::detail::normalizeAwaitableErrno(registered)));
        }

        C_IOResult result = wait(timeout_ms);
        if (result.code == C_IOResultTimeout) {
            markWaitTimeout();
        }
        if (hasPendingToken()) {
            const int removed = static_cast<IOScheduler*>(m_scheduler)->remove(controller);
            if (removed < 0) {
                result = merge_cleanup_result(
                    result,
                    make_result(C_IOResultError,
                                galay::kernel::detail::normalizeAwaitableErrno(removed)));
            }
            result = finishWithoutWait(result);
        }
        if (controller->m_awaitable[IOController::READ] == &m_awaitable) {
            controller->removeAwaitable(FILEREAD);
        }
        return result;
    }

    bool completeFromWake()
    {
        if (m_finished) {
            return true;
        }
        CoroAioCommitPhase expected = CoroAioCommitPhase::Pending;
        if (!m_phase.compare_exchange_strong(expected,
                                             CoroAioCommitPhase::Completed,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            if (expected == CoroAioCommitPhase::TimedOut) {
                m_finished = true;
                releaseUserDataOnly();
                return true;
            }
            return expected == CoroAioCommitPhase::Completed;
        }

        m_finished = true;
        C_IOResult completed = completeAndReleaseUserData(buildResult());
        return completed.code == C_IOResultOk || completed.code == C_IOResultInvalid;
    }

    Scheduler* scheduler() const noexcept { return m_scheduler; }

    C_IOResult wait(int64_t timeout_ms) noexcept
    {
        return m_wait_ops.wait != nullptr
            ? m_wait_ops.wait(m_wait_ops.ctx, timeout_ms)
            : make_result(C_IOResultInvalid);
    }

    bool hasPendingToken()
    {
        std::lock_guard<std::mutex> lock(m_user_data_mutex);
        return m_user_data != nullptr;
    }

    void markWaitTimeout() noexcept
    {
        CoroAioCommitPhase expected = CoroAioCommitPhase::Pending;
        if (!m_phase.compare_exchange_strong(expected,
                                             CoroAioCommitPhase::TimedOut,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return;
        }
    }

private:
    C_IOResult buildResult()
    {
        auto resumed = m_awaitable.await_resume();
        if (!resumed) {
            return from_io_error(resumed.error());
        }
        if (resumed->size() > m_result_capacity) {
            return make_result(C_IOResultInvalid);
        }
        *m_out_count = resumed->size();
        for (size_t i = 0; i < resumed->size(); ++i) {
            m_results[i] = (*resumed)[i];
        }
        return make_result(C_IOResultOk);
    }

    C_IOResult finishWithoutWait(C_IOResult result)
    {
        m_finished = true;
        C_IOResult completed = completeAndReleaseUserData(result);
        return merge_cleanup_result(result, completed);
    }

    C_IOResult completeAndReleaseUserData(C_IOResult result) noexcept
    {
        void* user_data = nullptr;
        C_IOResult completed = make_result(C_IOResultInvalid);
        {
            std::lock_guard<std::mutex> lock(m_user_data_mutex);
            user_data = std::exchange(m_user_data, nullptr);
            if (user_data != nullptr) {
                completed = m_wait_ops.complete_user_data(user_data, result);
            }
        }
        if (user_data != nullptr) {
            C_IOResult released = m_wait_ops.release_user_data(user_data);
            completed = merge_cleanup_result(completed, released);
        }
        return completed;
    }

    void releaseUserDataOnly() noexcept
    {
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_user_data_mutex);
            user_data = std::exchange(m_user_data, nullptr);
        }
        if (user_data != nullptr) {
            m_last_cleanup_result = m_wait_ops.release_user_data(user_data);
        }
    }

    static Scheduler* wake_owner(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroAioCommitWakeState*>(state);
        return wake_state != nullptr && wake_state->operation != nullptr
            ? wake_state->operation->scheduler()
            : nullptr;
    }

    static bool wake_request(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroAioCommitWakeState*>(state);
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

    AioCommitAwaitable m_awaitable;
    Scheduler* m_scheduler = nullptr;
    GalayCoreCoroWaitOps m_wait_ops{};
    void* m_user_data = nullptr;
    ssize_t* m_results = nullptr;
    size_t m_result_capacity = 0;
    size_t* m_out_count = nullptr;
    std::mutex m_user_data_mutex;
    std::atomic<CoroAioCommitPhase> m_phase{CoroAioCommitPhase::Pending};
    CoroAioCommitWakeState m_wake_state{};
    bool m_finished = false;
    C_IOResult m_last_cleanup_result = make_result(C_IOResultOk);
};

#endif

} // namespace

extern "C" {

GalayCoreCoroIOResult galay_core_coro_aio_file_commit(void* file_handle [[maybe_unused]],
                                                      void* scheduler_handle [[maybe_unused]],
                                                      ssize_t* results [[maybe_unused]],
                                                      size_t result_capacity [[maybe_unused]],
                                                      size_t* out_count [[maybe_unused]],
                                                      int64_t timeout_ms [[maybe_unused]],
                                                      void* user_data [[maybe_unused]],
                                                      const GalayCoreCoroWaitOps* wait_ops [[maybe_unused]])
{
#ifdef USE_EPOLL
    auto* file = to_cpp_file(file_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (file == nullptr || results == nullptr || result_capacity == 0 ||
        out_count == nullptr || scheduler == nullptr ||
        !timeout_fits_chrono(timeout_ms) || !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr || !file->isValid()) {
        return make_result(C_IOResultInvalid);
    }
    *out_count = 0;

    CoroAioCommitOperation operation(file->commit(),
                                     scheduler,
                                     user_data,
                                     *wait_ops,
                                     results,
                                     result_capacity,
                                     out_count);
    return operation.submitAndWait(timeout_ms);
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

} // extern "C"
