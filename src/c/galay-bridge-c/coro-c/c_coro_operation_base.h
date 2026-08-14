#ifndef GALAY_KERNEL_CORE_C_CORO_OPERATION_BASE_H
#define GALAY_KERNEL_CORE_C_CORO_OPERATION_BASE_H

/**
 * @file c_coro_operation_base.h
 * @brief direct C coroutine bridge 操作的共享基础设施。
 *
 * @details tcp/udp/async-file/file-watcher 四个 bridge 此前各自复制一份几乎相同的
 * 完成状态机（phase 原子 + user_data 原子 + finished 标志 + wake hooks + 注册/等待/
 * 清理流程）。本头文件把该状态机收敛为一份 CRTP 基类，并做出如下紧凑化：
 * - phase 与 finished 合并进单个 std::atomic<uint8_t>（CoroPhase 枚举态），完成路径
 *   从多次 RMW 降为一次 CAS，并消除 finished 标志的跨线程非原子读写；
 * - buildResult/commit/rollback 通过 CRTP 静态分发，不再需要虚调用；
 * - 操作结束后无条件清空 controller 的 owner scheduler：提交入口保证同一 controller
 *   同时至多存在一个 direct C coroutine 操作（读写双槽均空闲才放行），因此操作退出时
 *   不可能还有其它 direct 操作存活，与旧实现的 dynamic_cast 扫描语义等价且免 RTTI。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/timer_scheduler.h>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

namespace galay::bridge::detail
{

using galay::kernel::IOController;
using galay::kernel::IOError;
using galay::kernel::IOScheduler;
using galay::kernel::Scheduler;

using ::IOEventType;
using ::ACCEPT;
using ::CONNECT;
using ::RECV;
using ::SEND;
using ::READV;
using ::WRITEV;
using ::SENDFILE;
using ::FILEREAD;
using ::FILEWRITE;
using ::FILEWATCH;
using ::RECVFROM;
using ::SENDTO;

using C_IOResult = GalayCoreCoroIOResult;
using C_IOResultCode = GalayCoreCoroIOResultCode;

constexpr C_IOResultCode C_IOResultOk = GalayCoreCoroIOResultOk;
constexpr C_IOResultCode C_IOResultEof = GalayCoreCoroIOResultEof;
constexpr C_IOResultCode C_IOResultTimeout = GalayCoreCoroIOResultTimeout;
constexpr C_IOResultCode C_IOResultCancelled = GalayCoreCoroIOResultCancelled;
constexpr C_IOResultCode C_IOResultInvalid = GalayCoreCoroIOResultInvalid;
constexpr C_IOResultCode C_IOResultError = GalayCoreCoroIOResultError;

inline C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

inline C_IOResult merge_cleanup_result(C_IOResult primary, C_IOResult cleanup)
{
    return primary.code == C_IOResultOk && cleanup.code != C_IOResultOk
        ? cleanup
        : primary;
}

inline int io_error_sys_errno(const IOError& error)
{
    return static_cast<int>(error.code() >> 32U);
}

inline C_IOResult from_io_error(const IOError& error)
{
    if (IOError::contains(error.code(), galay::kernel::kParamInvalid) ||
        IOError::contains(error.code(), galay::kernel::kNotRunningOnIOScheduler) ||
        IOError::contains(error.code(), galay::kernel::kNotReady)) {
        return make_result(C_IOResultInvalid, io_error_sys_errno(error));
    }
    if (IOError::contains(error.code(), galay::kernel::kTimeout)) {
        return make_result(C_IOResultTimeout, io_error_sys_errno(error));
    }
    if (IOError::contains(error.code(), galay::kernel::kDisconnectError)) {
        return make_result(C_IOResultEof, io_error_sys_errno(error));
    }
    return make_result(C_IOResultError, io_error_sys_errno(error));
}

inline Scheduler* to_io_scheduler(GalayCoreIOScheduler* scheduler_handle)
{
    auto* scheduler = reinterpret_cast<Scheduler*>(scheduler_handle);
    return scheduler != nullptr && scheduler->type() == galay::kernel::kIOScheduler
        ? scheduler
        : nullptr;
}

inline bool valid_wait_ops(const GalayCoreCoroWaitOps* wait_ops)
{
    return wait_ops != nullptr &&
        wait_ops->wait != nullptr &&
        wait_ops->complete_user_data != nullptr &&
        wait_ops->release_user_data != nullptr;
}

inline IOController::Index slot_for_event(IOEventType event)
{
    switch (event) {
    case ACCEPT:
    case RECV:
    case READV:
    case RECVFROM:
    case FILEREAD:
    case FILEWATCH:
        return IOController::READ;
    case CONNECT:
    case SEND:
    case WRITEV:
    case SENDTO:
    case FILEWRITE:
    case SENDFILE:
        return IOController::WRITE;
    default:
        return IOController::SIZE;
    }
}

inline bool timeout_fits_chrono(int64_t timeout_ms)
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

inline bool validate_controller_owner(IOController* controller, Scheduler* scheduler)
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

/**
 * @brief direct C coroutine 操作的公共多态接口。
 * @details 仅服务 close/cancel 等冷路径（dynamic_cast 分发）；热路径全部走
 * CoroOperationBase 的静态 CRTP 分发，不经过虚函数。
 */
struct CoroOperationInterface {
    virtual ~CoroOperationInterface() = default;
    virtual void cancelFromClose() noexcept = 0;
    virtual Scheduler* scheduler() const noexcept = 0;
};

/**
 * @brief 合并后的操作阶段编码。
 * @details 高值态是"已定稿"（finished）态：任何定稿态都不会再被推进。
 */
enum CoroPhase : uint8_t {
    CoroPhasePending = 0,        ///< 已提交，等待 I/O。
    CoroPhaseIoCompleting = 1,   ///< reactor 正在执行 handleComplete。
    CoroPhaseTimedOut = 2,       ///< 等待侧已判定超时，等待撤销。
    CoroPhaseCancelled = 3,      ///< close 路径已取消，等待撤销。
    CoroPhaseCompleted = 4,      ///< I/O 已完成，等待唤醒路径定稿。
    CoroPhaseFinishedPending = 5,    ///< 已定稿（未完成的撤销路径）。
    CoroPhaseFinishedTimedOut = 6,   ///< 已定稿（超时路径）。
    CoroPhaseFinishedCancelled = 7,  ///< 已定稿（取消路径）。
    CoroPhaseFinishedCompleted = 8,  ///< 已定稿（正常完成路径）。
};

template <typename Derived>
struct CoroOperationBase: public CoroOperationInterface {
    struct WakeState {
        galay::kernel::detail::ResumeTokenHeader header;
        Derived* operation = nullptr;
    };

    CoroOperationBase(Scheduler* scheduler,
                      void* user_data,
                      GalayCoreCoroWaitOps wait_ops)
        : m_wait_ops(wait_ops)
        , m_scheduler(scheduler)
        , m_user_data(user_data)
    {
        m_wake_state.header.hooks = &kWakeHooks;
    }

    galay::kernel::Waker makeWaker() noexcept
    {
        // Derived construction has reached its body before makeWaker is called, so
        // this CRTP downcast no longer occurs while only the base subobject exists.
        m_wake_state.operation = static_cast<Derived*>(this);
        return galay::kernel::Waker(
            galay::kernel::detail::ResumeToken::fromNonOwningCCoroutine(&m_wake_state));
    }

    bool completeFromWake() noexcept
    {
        uint8_t phase = m_phase.load(std::memory_order_acquire);
        for (;;) {
            if (phase >= CoroPhaseFinishedPending) {
                return true;
            }
            if (phase == CoroPhasePending) {
                // 防御性转换：唤醒路径没有经过 handleComplete（与旧 UDP 语义一致）。
                uint8_t expected = CoroPhasePending;
                if (m_phase.compare_exchange_strong(expected,
                                                     CoroPhaseCompleted,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                    phase = CoroPhaseCompleted;
                    continue;
                }
                phase = expected;
                continue;
            }
            if (phase == CoroPhaseTimedOut || phase == CoroPhaseCancelled) {
                const uint8_t finished = phase == CoroPhaseTimedOut
                    ? CoroPhaseFinishedTimedOut
                    : CoroPhaseFinishedCancelled;
                uint8_t expected = phase;
                if (m_phase.compare_exchange_strong(expected,
                                                     finished,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                    releaseUserDataOnly();
                    static_cast<Derived*>(this)->rollbackResultImpl();
                    return true;
                }
                phase = expected;
                continue;
            }
            if (phase == CoroPhaseCompleted) {
                uint8_t expected = CoroPhaseCompleted;
                if (m_phase.compare_exchange_strong(expected,
                                                     CoroPhaseFinishedCompleted,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                    C_IOResult result = static_cast<Derived*>(this)->buildResultImpl();
                    if (result.code == C_IOResultOk) {
                        static_cast<Derived*>(this)->commitResultImpl();
                    } else {
                        static_cast<Derived*>(this)->rollbackResultImpl();
                    }
                    // complete_user_data may resume the C coroutine immediately and
                    // destroy this stack operation. It must be the final member call.
                    C_IOResult completed = completeAndReleaseUserData(result);
                    return completed.code == C_IOResultOk ||
                        completed.code == C_IOResultInvalid;
                }
                phase = expected;
                continue;
            }
            // IoCompleting：reactor 正在推进，本次唤醒请求交由定稿 CAS 处理。
            return true;
        }
    }

    C_IOResult immediateResult() noexcept
    {
        markFinished();
        return static_cast<Derived*>(this)->buildResultImpl();
    }

    C_IOResult finishWithoutWait(C_IOResult result) noexcept
    {
        markFinished();
        if (result.code == C_IOResultOk) {
            static_cast<Derived*>(this)->commitResultImpl();
        } else {
            static_cast<Derived*>(this)->rollbackResultImpl();
        }
        // Keep the completion callback last for the same immediate-resume lifetime
        // rule as completeFromWake.
        C_IOResult completed = completeAndReleaseUserData(result);
        return merge_cleanup_result(result, completed);
    }

    void cancelFromClose() noexcept override
    {
        if (finished()) {
            return;
        }
        uint8_t expected = CoroPhasePending;
        if (m_phase.compare_exchange_strong(expected,
                                             CoroPhaseCancelled,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            (void)completeUserDataNoRelease(make_result(C_IOResultCancelled));
        }
    }

    Scheduler* scheduler() const noexcept override { return m_scheduler; }

    bool hasPendingToken() noexcept
    {
        return m_user_data.load(std::memory_order_acquire) != nullptr;
    }

    C_IOResult wait(int64_t timeout_ms) noexcept
    {
        return m_wait_ops.wait != nullptr
            ? m_wait_ops.wait(m_wait_ops.ctx, timeout_ms)
            : make_result(C_IOResultInvalid);
    }

    void markWaitTimeout() noexcept
    {
        uint8_t expected = CoroPhasePending;
        (void)m_phase.compare_exchange_strong(expected,
                                              CoroPhaseTimedOut,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

protected:
    template <typename Fn>
    bool guardedHandleComplete(Fn&& fn) noexcept
    {
        uint8_t expected = CoroPhasePending;
        if (!m_phase.compare_exchange_strong(expected,
                                             CoroPhaseIoCompleting,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            if (expected == CoroPhaseTimedOut || expected == CoroPhaseCancelled) {
                return false;
            }
            return expected >= CoroPhaseCompleted;
        }
        const bool completed = fn();
        m_phase.store(completed ? CoroPhaseCompleted : CoroPhasePending,
                      std::memory_order_release);
        return completed;
    }

    void commitResultImpl() noexcept {}
    void rollbackResultImpl() noexcept {}

private:
    bool finished() const noexcept
    {
        return m_phase.load(std::memory_order_acquire) >= CoroPhaseFinishedPending;
    }

    void markFinished() noexcept
    {
        for (;;) {
            const uint8_t phase = m_phase.load(std::memory_order_acquire);
            if (phase >= CoroPhaseFinishedPending) {
                return;
            }
            if (phase == CoroPhaseIoCompleting) {
                // reactor 正在执行 handleComplete：短暂让出，等待其落定后再定稿。
                std::this_thread::yield();
                continue;
            }
            const uint8_t next = static_cast<uint8_t>(
                phase + (CoroPhaseFinishedPending - CoroPhasePending));
            uint8_t expected = phase;
            if (m_phase.compare_exchange_strong(expected,
                                                next,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                return;
            }
        }
    }

    C_IOResult completeUserDataNoRelease(C_IOResult result) noexcept
    {
        void* user_data = m_user_data.load(std::memory_order_acquire);
        if (user_data == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        return m_wait_ops.complete_user_data(user_data, result);
    }

    C_IOResult completeAndReleaseUserData(C_IOResult result) noexcept
    {
        const auto complete_user_data = m_wait_ops.complete_user_data;
        const auto release_user_data = m_wait_ops.release_user_data;
        C_IOResult completed = make_result(C_IOResultInvalid);
        void* user_data = m_user_data.exchange(nullptr, std::memory_order_acq_rel);
        if (user_data != nullptr && complete_user_data != nullptr) {
            completed = complete_user_data(user_data, result);
        }
        if (user_data != nullptr && release_user_data != nullptr) {
            C_IOResult released = release_user_data(user_data);
            completed = merge_cleanup_result(completed, released);
        }
        return completed;
    }

    void releaseUserDataOnly() noexcept
    {
        void* user_data = m_user_data.exchange(nullptr, std::memory_order_acq_rel);
        if (user_data != nullptr) {
            (void)m_wait_ops.release_user_data(user_data);
        }
    }

    static Scheduler* wake_owner(void* state) noexcept
    {
        auto* wake_state = static_cast<WakeState*>(state);
        return wake_state != nullptr && wake_state->operation != nullptr
            ? wake_state->operation->scheduler()
            : nullptr;
    }

    static bool wake_request(void* state) noexcept
    {
        auto* wake_state = static_cast<WakeState*>(state);
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

    WakeState m_wake_state{};
    GalayCoreCoroWaitOps m_wait_ops{};
    Scheduler* m_scheduler = nullptr;
    std::atomic<void*> m_user_data{nullptr};
    std::atomic<uint8_t> m_phase{CoroPhasePending};
};

/**
 * @brief 提交已填充的 awaitable 并等待完成。
 * @details tcp/udp/async-file 的注册-等待-清理主流程；file-watcher 通过
 * RequireOtherSlotEmpty=false 复用（只检查本槽位）。
 * 操作退出（含错误路径）后无条件清空 owner：提交入口保证同时至多一个 direct 操作。
 */
template <bool RequireOtherSlotEmpty,
          bool CloseOnConnectTimeout,
          typename Operation,
          typename Awaitable>
inline C_IOResult perform_registered_io(IOController* controller,
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
        (RequireOtherSlotEmpty &&
         (controller->m_awaitable[other_slot] != nullptr ||
          controller->m_sequence_owner[other_slot] != nullptr))) {
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }
    if (!validate_controller_owner(controller, scheduler)) {
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }
    if (!controller->fillAwaitable(event, awaitable)) {
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
        return operation.finishWithoutWait(make_result(C_IOResultInvalid));
    }
    if (timeout_ms > 0 && !galay::kernel::TimerScheduler::getInstance()->isRunning()) {
        controller->removeAwaitable(event);
        C_IOResult result = operation.finishWithoutWait(make_result(C_IOResultError));
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
        return result;
    }

    const int registered = galay::kernel::detail::registerIOSchedulerEvent(
        scheduler, event, controller);
    if (registered == 1) {
        controller->removeAwaitable(event);
        C_IOResult result = operation.immediateResult();
        result = operation.finishWithoutWait(result);
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
        return result;
    }
    if (registered < 0) {
        controller->removeAwaitable(event);
        C_IOResult result = operation.finishWithoutWait(
            make_result(C_IOResultError, galay::kernel::detail::normalizeAwaitableErrno(registered)));
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
        return result;
    }

    C_IOResult result = operation.wait(timeout_ms);
    if (result.code == C_IOResultTimeout) {
        operation.markWaitTimeout();
    }
    if constexpr (CloseOnConnectTimeout) {
        if (result.code == C_IOResultTimeout && event == CONNECT) {
            if (operation.hasPendingToken()) {
                result = operation.finishWithoutWait(result);
            }
            const int closed = galay::kernel::detail::registerIOSchedulerClose(scheduler, controller);
            if (closed == 0) {
                controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
                return result;
            }
            return make_result(C_IOResultError,
                               galay::kernel::detail::normalizeAwaitableErrno(closed));
        }
    }
    if (operation.hasPendingToken()) {
#ifdef USE_IOURING
        const bool keep_persistent_multishot =
            (event == ACCEPT || event == RECV || event == RECVFROM) &&
            (result.code == C_IOResultTimeout || result.code == C_IOResultCancelled);
        if (!keep_persistent_multishot) {
            const int removed = static_cast<IOScheduler*>(scheduler)->remove(controller);
            if (removed < 0) {
                result = merge_cleanup_result(
                    result,
                    make_result(C_IOResultError,
                                galay::kernel::detail::normalizeAwaitableErrno(removed)));
            }
        }
#else
        const int removed = static_cast<IOScheduler*>(scheduler)->remove(controller);
        if (removed < 0) {
            result = merge_cleanup_result(
                result,
                make_result(C_IOResultError,
                            galay::kernel::detail::normalizeAwaitableErrno(removed)));
        }
#endif
        result = operation.finishWithoutWait(result);
    }
    if (controller->m_awaitable[slot] == awaitable) {
        controller->removeAwaitable(event);
    }
    controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
    return result;
}

inline bool is_direct_coro_operation(void* awaitable)
{
    if (awaitable == nullptr) {
        return false;
    }
    auto* base = static_cast<galay::kernel::AwaitableBase*>(awaitable);
    return dynamic_cast<CoroOperationInterface*>(base) != nullptr;
}

inline bool has_non_direct_pending_operation(IOController* controller)
{
    if (controller == nullptr) {
        return true;
    }
    if (controller->m_sequence_owner[IOController::READ] != nullptr ||
        controller->m_sequence_owner[IOController::WRITE] != nullptr) {
        return true;
    }
    void* read = controller->m_awaitable[IOController::READ];
    if (read != nullptr && !is_direct_coro_operation(read)) {
        return true;
    }
    void* write = controller->m_awaitable[IOController::WRITE];
    return write != nullptr && !is_direct_coro_operation(write);
}

inline void cancel_coro_operation(void* awaitable)
{
    if (awaitable == nullptr) {
        return;
    }
    auto* base = static_cast<galay::kernel::AwaitableBase*>(awaitable);
    auto* operation = dynamic_cast<CoroOperationInterface*>(base);
    if (operation != nullptr) {
        operation->cancelFromClose();
    }
}

inline void cancel_coro_operations(IOController* controller)
{
    cancel_coro_operation(controller->m_awaitable[IOController::READ]);
    cancel_coro_operation(controller->m_awaitable[IOController::WRITE]);
}

inline Scheduler* pending_coro_operation_scheduler(void* awaitable)
{
    if (awaitable == nullptr) {
        return nullptr;
    }
    auto* base = static_cast<galay::kernel::AwaitableBase*>(awaitable);
    auto* operation = dynamic_cast<CoroOperationInterface*>(base);
    return operation != nullptr ? operation->scheduler() : nullptr;
}

/**
 * @brief close 入口：校验所有权、取消挂起的 direct 操作并注册关闭。
 */
inline C_IOResult perform_coro_close(IOController* controller, Scheduler* scheduler)
{
    if (has_non_direct_pending_operation(controller)) {
        return make_result(C_IOResultInvalid);
    }
    if (!validate_controller_owner(controller, scheduler)) {
        return make_result(C_IOResultInvalid);
    }
    Scheduler* pending_scheduler =
        pending_coro_operation_scheduler(controller->m_awaitable[IOController::READ]);
    if (pending_scheduler == nullptr) {
        pending_scheduler =
            pending_coro_operation_scheduler(controller->m_awaitable[IOController::WRITE]);
    }
    if (pending_scheduler != nullptr && pending_scheduler != scheduler) {
        return make_result(C_IOResultInvalid);
    }
    cancel_coro_operations(controller);
    const int closed = galay::kernel::detail::registerIOSchedulerClose(scheduler, controller);
    if (closed == 0) {
        controller->m_owner_scheduler.store(nullptr, std::memory_order_release);
        return make_result(C_IOResultOk);
    }
    return make_result(C_IOResultError, galay::kernel::detail::normalizeAwaitableErrno(closed));
}

} // namespace galay::bridge::detail

#endif
