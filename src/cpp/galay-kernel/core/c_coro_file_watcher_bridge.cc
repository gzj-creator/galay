#include "c_coro_file_watcher_bridge.h"

#include "../async/file_watcher.h"
#include "awaitable.h"
#include "io_scheduler.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace
{

using C_IOResult = GalayCoreCoroIOResult;
using C_IOResultCode = GalayCoreCoroIOResultCode;
using galay::async::FileWatcher;
using galay::kernel::FileWatchAwaitable;
using galay::kernel::IOController;
using galay::kernel::IOError;
using galay::kernel::IOScheduler;
using galay::kernel::Scheduler;

constexpr C_IOResultCode C_IOResultOk = GalayCoreCoroIOResultOk;
constexpr C_IOResultCode C_IOResultTimeout = GalayCoreCoroIOResultTimeout;
constexpr C_IOResultCode C_IOResultCancelled = GalayCoreCoroIOResultCancelled;
constexpr C_IOResultCode C_IOResultInvalid = GalayCoreCoroIOResultInvalid;
constexpr C_IOResultCode C_IOResultError = GalayCoreCoroIOResultError;

struct CoroFileWatcherOperation;

struct CoroFileWatcherWakeState {
    galay::kernel::detail::ResumeTokenHeader header;
    CoroFileWatcherOperation* operation = nullptr;
};

enum class CoroFileWatcherCompletionPhase : uint8_t {
    Pending,
    IoCompleting,
    TimedOut,
    Cancelled,
    Completed,
};

struct CoroFileWatcherCompletionState {
    std::atomic<CoroFileWatcherCompletionPhase> phase{CoroFileWatcherCompletionPhase::Pending};
    std::mutex user_data_mutex;
    void* user_data = nullptr;
    GalayCoreCoroWaitOps wait_ops{};
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

FileWatcher* to_cpp_watcher(void* watcher)
{
    return static_cast<FileWatcher*>(watcher);
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

void copy_name_to_result(const std::string& name, GalayCoreCoroFileWatchResult* result)
{
    const auto bytes = name.size() < sizeof(result->name) - 1
        ? name.size()
        : sizeof(result->name) - 1;
    if (bytes > 0) {
        std::memcpy(result->name, name.data(), bytes);
    }
    result->name[bytes] = '\0';
}

bool validate_controller_owner(IOController* controller, Scheduler* scheduler)
{
    if (controller == nullptr || scheduler == nullptr) {
        return false;
    }
    Scheduler* expected = nullptr;
    if (controller->m_owner_scheduler.compare_exchange_strong(
            expected,
            scheduler,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return true;
    }
    return expected == scheduler ||
        controller->m_owner_scheduler.load(std::memory_order_acquire) == scheduler;
}

void clear_controller_owner_if_no_live_operation(IOController* controller) noexcept
{
    if (controller != nullptr && controller->m_awaitable[IOController::READ] == nullptr) {
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
    }
}

struct CoroFileWatcherOperation final: public FileWatchAwaitable {
    CoroFileWatcherOperation(IOController* controller,
                             Scheduler* scheduler,
                             void* user_data,
                             GalayCoreCoroWaitOps wait_ops,
                             GalayCoreCoroFileWatchResult* out_result)
#ifdef USE_KQUEUE
        : FileWatchAwaitable(controller,
                             m_buffer,
                             sizeof(m_buffer),
                             galay::kernel::FileWatchEvent::All)
#else
        : FileWatchAwaitable(controller, m_buffer, sizeof(m_buffer))
#endif
        , m_scheduler(scheduler)
        , m_wait_ops(wait_ops)
        , m_out_result(out_result)
    {
        m_state.user_data = user_data;
        m_state.wait_ops = wait_ops;
        m_wake_state.header.hooks = &kWakeHooks;
        m_wake_state.operation = this;
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = FILEWATCH;
#endif
    }

    galay::kernel::Waker makeWaker() noexcept
    {
        return galay::kernel::Waker(
            galay::kernel::detail::ResumeToken::fromCCoroutine(&m_wake_state));
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileWatchAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileWatchAwaitable::handleComplete(handle); });
    }
#endif

    bool completeFromWake() noexcept
    {
        if (m_finished) {
            return true;
        }
        const CoroFileWatcherCompletionPhase phase =
            m_state.phase.load(std::memory_order_acquire);
        if (phase == CoroFileWatcherCompletionPhase::TimedOut ||
            phase == CoroFileWatcherCompletionPhase::Cancelled) {
            m_finished = true;
            releaseUserDataOnly();
            return true;
        }
        if (phase != CoroFileWatcherCompletionPhase::Completed) {
            return true;
        }
        m_finished = true;
        return completeUserData(buildResult());
    }

    C_IOResult immediateResult() noexcept
    {
        m_finished = true;
        return buildResult();
    }

    C_IOResult finishWithoutWait(C_IOResult result) noexcept
    {
        m_finished = true;
        C_IOResult completed = completeAndReleaseUserData(result);
        return merge_cleanup_result(result, completed);
    }

    Scheduler* scheduler() const noexcept { return m_scheduler; }

    bool hasPendingToken() noexcept
    {
        std::lock_guard<std::mutex> lock(m_state.user_data_mutex);
        return m_state.user_data != nullptr;
    }

    C_IOResult wait(int64_t timeout_ms) noexcept
    {
        return m_wait_ops.wait != nullptr
            ? m_wait_ops.wait(m_wait_ops.ctx, timeout_ms)
            : make_result(C_IOResultInvalid);
    }

    void markWaitTimeout() noexcept
    {
        CoroFileWatcherCompletionPhase expected = CoroFileWatcherCompletionPhase::Pending;
        if (!m_state.phase.compare_exchange_strong(expected,
                                                   CoroFileWatcherCompletionPhase::TimedOut,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            return;
        }
    }

private:
    template <typename Fn>
    bool guardedHandleComplete(Fn&& fn) noexcept
    {
        CoroFileWatcherCompletionPhase expected = CoroFileWatcherCompletionPhase::Pending;
        if (!m_state.phase.compare_exchange_strong(expected,
                                                   CoroFileWatcherCompletionPhase::IoCompleting,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            return expected == CoroFileWatcherCompletionPhase::Completed;
        }
        const bool completed = fn();
        m_state.phase.store(completed ? CoroFileWatcherCompletionPhase::Completed
                                      : CoroFileWatcherCompletionPhase::Pending,
                            std::memory_order_release);
        return completed;
    }

    C_IOResult buildResult() noexcept
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        GalayCoreCoroFileWatchResult result{};
        result.events = static_cast<GalayCoreCoroFileWatchEvent>(
            static_cast<unsigned int>(m_result->event));
        result.is_dir = m_result->isDir;
        copy_name_to_result(m_result->name, &result);
        *m_out_result = result;
        return make_result(C_IOResultOk);
    }

    bool completeUserData(C_IOResult result) noexcept
    {
        const C_IOResult completed = completeAndReleaseUserData(result);
        return completed.code == C_IOResultOk || completed.code == C_IOResultInvalid;
    }

    C_IOResult completeAndReleaseUserData(C_IOResult result) noexcept
    {
        void* user_data = nullptr;
        C_IOResult completed = make_result(C_IOResultInvalid);
        {
            std::lock_guard<std::mutex> lock(m_state.user_data_mutex);
            user_data = std::exchange(m_state.user_data, nullptr);
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
            std::lock_guard<std::mutex> lock(m_state.user_data_mutex);
            user_data = std::exchange(m_state.user_data, nullptr);
        }
        if (user_data != nullptr) {
            m_last_cleanup_result = m_wait_ops.release_user_data(user_data);
        }
    }

    static Scheduler* wake_owner(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroFileWatcherWakeState*>(state);
        return wake_state != nullptr && wake_state->operation != nullptr
            ? wake_state->operation->scheduler()
            : nullptr;
    }

    static bool wake_request(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroFileWatcherWakeState*>(state);
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

    Scheduler* m_scheduler = nullptr;
    GalayCoreCoroWaitOps m_wait_ops{};
    CoroFileWatcherCompletionState m_state;
    CoroFileWatcherWakeState m_wake_state{};
    GalayCoreCoroFileWatchResult* m_out_result = nullptr;
    char m_buffer[4096]{};
    bool m_finished = false;
    C_IOResult m_last_cleanup_result = make_result(C_IOResultOk);
};

C_IOResult perform_registered_watch(IOController* controller,
                                    Scheduler* scheduler,
                                    int64_t timeout_ms,
                                    CoroFileWatcherOperation& operation)
{
    if (controller == nullptr || scheduler == nullptr ||
        controller->m_awaitable[IOController::READ] != nullptr ||
        controller->m_sequence_owner[IOController::READ] != nullptr ||
        !validate_controller_owner(controller, scheduler)) {
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }
    if (!controller->fillAwaitable(FILEWATCH, static_cast<FileWatchAwaitable*>(&operation))) {
        clear_controller_owner_if_no_live_operation(controller);
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }

    const int registered =
        galay::kernel::detail::registerIOSchedulerEvent(scheduler, FILEWATCH, controller);
    if (registered == 1) {
        controller->removeAwaitable(FILEWATCH);
        C_IOResult result = operation.immediateResult();
        result = operation.finishWithoutWait(result);
        clear_controller_owner_if_no_live_operation(controller);
        return result;
    }
    if (registered < 0) {
        controller->removeAwaitable(FILEWATCH);
        C_IOResult result = operation.finishWithoutWait(
            make_result(C_IOResultError, galay::kernel::detail::normalizeAwaitableErrno(registered)));
        clear_controller_owner_if_no_live_operation(controller);
        return result;
    }

    C_IOResult result = operation.wait(timeout_ms);
    if (result.code == C_IOResultTimeout) {
        operation.markWaitTimeout();
    }
    if (operation.hasPendingToken()) {
        const int removed = static_cast<IOScheduler*>(scheduler)->remove(controller);
        if (removed < 0) {
            result = merge_cleanup_result(
                result,
                make_result(C_IOResultError,
                            galay::kernel::detail::normalizeAwaitableErrno(removed)));
        }
        result = operation.finishWithoutWait(result);
    }
    if (controller->m_awaitable[IOController::READ] == static_cast<FileWatchAwaitable*>(&operation)) {
        controller->removeAwaitable(FILEWATCH);
    }
    clear_controller_owner_if_no_live_operation(controller);
    return result;
}

} // namespace

extern "C" {

GalayCoreCoroIOResult galay_core_coro_file_watcher_watch(
    void* watcher_handle,
    void* scheduler_handle,
    GalayCoreCoroFileWatchResult* out_result,
    int64_t timeout_ms,
    void* user_data,
    const GalayCoreCoroWaitOps* wait_ops)
{
    auto* watcher = to_cpp_watcher(watcher_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (watcher == nullptr || out_result == nullptr ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr || !watcher->isValid()) {
        return make_result(C_IOResultInvalid);
    }
    CoroFileWatcherOperation operation(watcher->getController(),
                                       scheduler,
                                       user_data,
                                       *wait_ops,
                                       out_result);
    return perform_registered_watch(watcher->getController(), scheduler, timeout_ms, operation);
}

} // extern "C"
