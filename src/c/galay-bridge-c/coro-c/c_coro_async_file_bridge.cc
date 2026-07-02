#include "c_coro_async_file_bridge.h"

#include <galay/cpp/galay-kernel/async/async_file.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <limits>
#include <mutex>
#include <utility>

namespace
{

using C_IOResult = GalayCoreCoroIOResult;
using C_IOResultCode = GalayCoreCoroIOResultCode;

constexpr C_IOResultCode C_IOResultOk = GalayCoreCoroIOResultOk;
constexpr C_IOResultCode C_IOResultTimeout = GalayCoreCoroIOResultTimeout;
constexpr C_IOResultCode C_IOResultCancelled = GalayCoreCoroIOResultCancelled;
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

#if defined(USE_KQUEUE) || defined(USE_IOURING)

using galay::async::AsyncFile;
using galay::kernel::FileReadAwaitable;
using galay::kernel::FileWriteAwaitable;
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

AsyncFile* to_cpp_file(GalayCoreAsyncFile* file)
{
    return reinterpret_cast<AsyncFile*>(file);
}

Scheduler* to_io_scheduler(GalayCoreIOScheduler* scheduler_handle)
{
    auto* scheduler = reinterpret_cast<Scheduler*>(scheduler_handle);
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

IOController::Index slot_for_event(IOEventType event)
{
    switch (event) {
    case FILEREAD:
        return IOController::READ;
    case FILEWRITE:
        return IOController::WRITE;
    default:
        return IOController::SIZE;
    }
}

struct CoroFileOperationBase;

struct CoroFileWakeState {
    galay::kernel::detail::ResumeTokenHeader header;
    CoroFileOperationBase* operation = nullptr;
};

enum class CoroFileCompletionPhase : uint8_t {
    Pending,
    IoCompleting,
    TimedOut,
    Completed,
};

struct CoroFileOperationBase {
    CoroFileOperationBase(Scheduler* scheduler,
                          void* user_data,
                          GalayCoreCoroWaitOps wait_ops)
        : m_scheduler(scheduler)
        , m_wait_ops(wait_ops)
        , m_user_data(user_data)
    {
        m_wake_state.header.hooks = &kWakeHooks;
        m_wake_state.operation = this;
    }

    virtual ~CoroFileOperationBase() = default;

    galay::kernel::Waker makeWaker() noexcept
    {
        return galay::kernel::Waker(
            galay::kernel::detail::ResumeToken::fromCCoroutine(&m_wake_state));
    }

    bool completeFromWake() noexcept
    {
        if (m_finished) {
            return true;
        }
        const CoroFileCompletionPhase phase = m_phase.load(std::memory_order_acquire);
        if (phase == CoroFileCompletionPhase::TimedOut) {
            m_finished = true;
            releaseUserDataOnly();
            return true;
        }
        if (phase != CoroFileCompletionPhase::Completed) {
            return true;
        }
        m_finished = true;
        C_IOResult completed = completeAndReleaseUserData(buildResult());
        return completed.code == C_IOResultOk || completed.code == C_IOResultInvalid;
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
        std::lock_guard<std::mutex> lock(m_user_data_mutex);
        return m_user_data != nullptr;
    }

    C_IOResult wait(int64_t timeout_ms) noexcept
    {
        return m_wait_ops.wait != nullptr
            ? m_wait_ops.wait(m_wait_ops.ctx, timeout_ms)
            : make_result(C_IOResultInvalid);
    }

    void markWaitTimeout() noexcept
    {
        CoroFileCompletionPhase expected = CoroFileCompletionPhase::Pending;
        if (!m_phase.compare_exchange_strong(expected,
                                             CoroFileCompletionPhase::TimedOut,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return;
        }
    }

protected:
    template <typename Fn>
    bool guardedHandleComplete(Fn&& fn) noexcept
    {
        CoroFileCompletionPhase expected = CoroFileCompletionPhase::Pending;
        if (!m_phase.compare_exchange_strong(expected,
                                             CoroFileCompletionPhase::IoCompleting,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return expected == CoroFileCompletionPhase::Completed;
        }
        const bool completed = fn();
        m_phase.store(completed ? CoroFileCompletionPhase::Completed
                                : CoroFileCompletionPhase::Pending,
                      std::memory_order_release);
        return completed;
    }

private:
    virtual C_IOResult buildResult() = 0;

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
        auto* wake_state = static_cast<CoroFileWakeState*>(state);
        return wake_state != nullptr && wake_state->operation != nullptr
            ? wake_state->operation->scheduler()
            : nullptr;
    }

    static bool wake_request(void* state) noexcept
    {
        auto* wake_state = static_cast<CoroFileWakeState*>(state);
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
    void* m_user_data = nullptr;
    std::mutex m_user_data_mutex;
    std::atomic<CoroFileCompletionPhase> m_phase{CoroFileCompletionPhase::Pending};
    CoroFileWakeState m_wake_state{};
    bool m_finished = false;
    C_IOResult m_last_cleanup_result = make_result(C_IOResultOk);
};

struct CoroFileReadOperation final: public FileReadAwaitable, public CoroFileOperationBase {
    CoroFileReadOperation(IOController* controller,
                          Scheduler* scheduler,
                          void* user_data,
                          GalayCoreCoroWaitOps wait_ops,
                          char* buffer,
                          size_t length,
                          off_t offset)
        : FileReadAwaitable(controller, buffer, length, offset)
        , CoroFileOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = FILEREAD;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileReadAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileReadAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

struct CoroFileWriteOperation final: public FileWriteAwaitable, public CoroFileOperationBase {
    CoroFileWriteOperation(IOController* controller,
                           Scheduler* scheduler,
                           void* user_data,
                           GalayCoreCoroWaitOps wait_ops,
                           const char* buffer,
                           size_t length,
                           off_t offset)
        : FileWriteAwaitable(controller, buffer, length, offset)
        , CoroFileOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = FILEWRITE;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileWriteAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileWriteAwaitable::handleComplete(handle); });
    }
#endif

private:
    C_IOResult buildResult() override
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

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

void clear_controller_owner_if_idle(IOController* controller) noexcept
{
    if (controller != nullptr &&
        controller->m_awaitable[IOController::READ] == nullptr &&
        controller->m_awaitable[IOController::WRITE] == nullptr) {
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
    }
}

template <typename Operation, typename Awaitable>
C_IOResult perform_registered_io(IOController* controller,
                                 Scheduler* scheduler,
                                 IOEventType event,
                                 int64_t timeout_ms,
                                 Operation& operation,
                                 Awaitable* awaitable)
{
    const IOController::Index slot = slot_for_event(event);
    const IOController::Index other_slot =
        slot == IOController::READ ? IOController::WRITE : IOController::READ;
    if (controller == nullptr || scheduler == nullptr || slot == IOController::SIZE ||
        controller->m_awaitable[slot] != nullptr ||
        controller->m_sequence_owner[slot] != nullptr ||
        controller->m_awaitable[other_slot] != nullptr ||
        controller->m_sequence_owner[other_slot] != nullptr) {
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }
    if (!validate_controller_owner(controller, scheduler)) {
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }
    if (!controller->fillAwaitable(event, awaitable)) {
        clear_controller_owner_if_idle(controller);
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }

    const int registered = galay::kernel::detail::registerIOSchedulerEvent(
        scheduler, event, controller);
    if (registered == 1) {
        controller->removeAwaitable(event);
        C_IOResult result = operation.immediateResult();
        result = operation.finishWithoutWait(result);
        clear_controller_owner_if_idle(controller);
        return result;
    }
    if (registered < 0) {
        controller->removeAwaitable(event);
        C_IOResult result = operation.finishWithoutWait(
            make_result(C_IOResultError, galay::kernel::detail::normalizeAwaitableErrno(registered)));
        clear_controller_owner_if_idle(controller);
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
    if (controller->m_awaitable[slot] == awaitable) {
        controller->removeAwaitable(event);
    }
    clear_controller_owner_if_idle(controller);
    return result;
}

#endif

} // namespace

extern "C" {

GalayCoreCoroIOResult galay_core_coro_async_file_read(GalayCoreAsyncFile* file_handle [[maybe_unused]],
                                                      GalayCoreIOScheduler* scheduler_handle [[maybe_unused]],
                                                      char* buffer [[maybe_unused]],
                                                      size_t length [[maybe_unused]],
                                                      int64_t offset [[maybe_unused]],
                                                      int64_t timeout_ms [[maybe_unused]],
                                                      void* user_data [[maybe_unused]],
                                                      const GalayCoreCoroWaitOps* wait_ops [[maybe_unused]])
{
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    auto* file = to_cpp_file(file_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (file == nullptr || buffer == nullptr || length == 0 || offset < 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroFileReadOperation operation(file->getController(),
                                    scheduler,
                                    user_data,
                                    *wait_ops,
                                    buffer,
                                    length,
                                    static_cast<off_t>(offset));
    return perform_registered_io(file->getController(),
                                 scheduler,
                                 FILEREAD,
                                 timeout_ms,
                                 operation,
                                 static_cast<FileReadAwaitable*>(&operation));
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

GalayCoreCoroIOResult galay_core_coro_async_file_write(GalayCoreAsyncFile* file_handle [[maybe_unused]],
                                                       GalayCoreIOScheduler* scheduler_handle [[maybe_unused]],
                                                       const char* buffer [[maybe_unused]],
                                                       size_t length [[maybe_unused]],
                                                       int64_t offset [[maybe_unused]],
                                                       int64_t timeout_ms [[maybe_unused]],
                                                       void* user_data [[maybe_unused]],
                                                       const GalayCoreCoroWaitOps* wait_ops [[maybe_unused]])
{
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    auto* file = to_cpp_file(file_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (file == nullptr || buffer == nullptr || length == 0 || offset < 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroFileWriteOperation operation(file->getController(),
                                     scheduler,
                                     user_data,
                                     *wait_ops,
                                     buffer,
                                     length,
                                     static_cast<off_t>(offset));
    return perform_registered_io(file->getController(),
                                 scheduler,
                                 FILEWRITE,
                                 timeout_ms,
                                 operation,
                                 static_cast<FileWriteAwaitable*>(&operation));
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

GalayCoreCoroIOResult galay_core_coro_async_file_close(GalayCoreAsyncFile* file_handle,
                                                       GalayCoreIOScheduler* scheduler_handle,
                                                       int64_t)
{
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    auto* file = to_cpp_file(file_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (file == nullptr || scheduler == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    IOController* controller = file->getController();
    if (!validate_controller_owner(controller, scheduler)) {
        return make_result(C_IOResultInvalid);
    }
    const int closed = galay::kernel::detail::registerIOSchedulerClose(scheduler, controller);
    if (closed == 0) {
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
        return make_result(C_IOResultOk);
    }
    return make_result(C_IOResultError, galay::kernel::detail::normalizeAwaitableErrno(closed));
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

} // extern "C"
