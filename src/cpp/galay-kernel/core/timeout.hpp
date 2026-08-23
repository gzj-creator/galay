/**
 * @file timeout.hpp
 * @brief 异步 IO awaitable 的超时支持
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供：
 * - TimeoutTimer：定时器子类，在超时时唤醒关联协程
 * - TimeoutSupport<Derived>：CRTP 混入，为任意 awaitable 添加 .timeout(ms) 方法
 * - WithTimeout<Awaitable>：组合 awaitable 与定时器的包装器，
 *   当内部操作未在规定时间内完成时注入超时错误
 */

#ifndef GALAY_KERNEL_TIMEOUT_HPP
#define GALAY_KERNEL_TIMEOUT_HPP

#include "../common/error.h"
#include "../common/timer.hpp"
#include "../common/concepts.h"
#include "io_controller.hpp"
#include "waker.h"
#include "scheduler.hpp"
#include "../common/kernel_config.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>
#include <expected>
#include <type_traits>
#include <thread>
#include <utility>

namespace galay::kernel 
{

class IOController;
class TimeoutTimer;

namespace detail {

/**
 * @brief 将完成唤醒延迟到 await_suspend 最后发布之后。
 * @details 完成方在注册尚处于 arming 阶段时只记录 pending；注册方随后通过
 *          arm() 决定真正挂起，或在已有 pending 完成时同步继续。该状态机保证
 *          协程不会在 await_suspend 仍访问 awaiter/channel 时被恢复并销毁。
 * @note setWaker() 只能在对象发布给并发完成方之前调用；arm() 只能调用一次。
 */
class DeferredWaker
{
public:
    DeferredWaker() noexcept = default;

    explicit DeferredWaker(Waker waker) noexcept
        : m_waker(std::move(waker))
    {
    }

    DeferredWaker(const DeferredWaker&) = delete;
    DeferredWaker& operator=(const DeferredWaker&) = delete;
    DeferredWaker(DeferredWaker&&) = delete;
    DeferredWaker& operator=(DeferredWaker&&) = delete;

    /** @brief 在发布给完成方之前设置目标协程唤醒器。 */
    void setWaker(Waker waker) noexcept
    {
        m_waker = std::move(waker);
    }

    /**
     * @brief 发布 await_suspend 已完成所有相关对象访问。
     * @return true 表示尚无完成事件，调用方应保持挂起；false 表示完成已提前到达，
     *         调用方应同步继续且不会再收到该 gate 的异步唤醒。
     */
    [[nodiscard]] bool arm() noexcept
    {
        State expected = State::kArming;
        if (m_state.compare_exchange_strong(expected,
                                            State::kArmed,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            return true;
        }
        if (expected == State::kPending) {
            // requestWake() 不会离开 kPending，只有注册方在这里消费该状态。
            m_state.store(State::kIssued, std::memory_order_release);
        }
        return false;
    }

    /**
     * @brief 请求完成当前等待。
     * @return 当前调用首次发布 pending 或实际发出唤醒时返回 true；重复请求返回 false。
     * @note 实际 wakeUp() 是成功路径的最后一步，调用后不再访问本对象。
     */
    [[nodiscard]] bool requestWake() noexcept
    {
        State state = m_state.load(std::memory_order_acquire);
        for (;;) {
            if (state == State::kArming) {
                if (m_state.compare_exchange_weak(state,
                                                  State::kPending,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                    return true;
                }
                continue;
            }
            if (state != State::kArmed) {
                return false;
            }

            Waker waker = m_waker;
            if (m_state.compare_exchange_weak(state,
                                              State::kIssued,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                waker.wakeUp();
                return true;
            }
        }
    }

    /** @brief awaiter 恢复后释放 gate 持有的任务引用。 */
    void clearWaker() noexcept
    {
        m_waker = Waker();
    }

private:
    enum class State : uint8_t {
        kArming,
        kArmed,
        kPending,
        kIssued,
    };

    Waker m_waker;
    std::atomic<State> m_state{State::kArming};
};

int removeTimedOutIORegistration(Scheduler* scheduler, IOController* controller) noexcept;

template <typename Awaitable>
bool awaitableStillOwnsIORegistration(Awaitable& awaitable) noexcept
{
    if constexpr (requires(Awaitable& value) {
        { value.ownsIoRegistration() } -> std::convertible_to<bool>;
    }) {
        // 显式定制点优先：inner 自身最清楚是否仍持有 IO 注册
        return awaitable.ownsIoRegistration();
    } else if constexpr (requires(Awaitable& value) { value.m_controller; }) {
        auto* controller = awaitable.m_controller;
        if (controller == nullptr) {
            return false;
        }
        if constexpr (requires(Awaitable& value) { value.m_registered; }) {
            if (awaitable.m_registered) {
                return true;
            }
        }
        const void* self = static_cast<const void*>(&awaitable);
        return controller->m_awaitable[IOController::READ] == self ||
               controller->m_awaitable[IOController::WRITE] == self;
    } else {
        return true;
    }
}

template <typename T>
struct timeout_expected_traits;

template <typename ValueT, typename ErrorT>
struct timeout_expected_traits<std::expected<ValueT, ErrorT>> {
    using error_type = ErrorT;
};

template <typename T>
inline constexpr bool timeout_always_false = false;

/**
 * @brief Compile-time timeout behavior for an awaitable.
 *
 * A policy is an empty type with a static `inject(Awaitable&)` function.  It
 * keeps custom timeout semantics explicit while allowing the compiler to
 * inline the operation completely.  The default policy is an empty tag; its
 * legacy adaptation is implemented inside the befriended `WithTimeout` wrapper
 * so existing private channel markers remain accessible without exposing them.
 */
template <typename Awaitable>
struct DefaultTimeoutPolicy {};

template <typename Policy, typename Awaitable>
concept TimeoutPolicy = requires(Awaitable& awaitable) {
    Policy::inject(awaitable);
};

}  // namespace detail

namespace detail {

/**
 * @brief TimeoutTimer 的调度线程本地回收池。
 *
 * acquire 返回清零后的复用对象或新建对象；属主线程释放时归还线程本地
 * free-list，跨线程最后释放则直接析构，避免把对象发布到错误的池。
 */
class TimeoutTimerPool
{
public:
    TimeoutTimerPool()
    {
        // release() 是不抛异常路径，因此一次性预留有界空闲列表，避免
        // vector 扩容在释放过程中分配内存。
        m_free.reserve(GALAY_KERNEL_TIMEOUT_TIMER_POOL_MAX_CACHED);
    }

    TimeoutTimer* acquire(std::chrono::milliseconds duration);
    void release(TimeoutTimer* timer) noexcept;
    ~TimeoutTimerPool();

private:
    std::vector<TimeoutTimer*> m_free;
};

inline TimeoutTimerPool& timeoutTimerPoolLocal() noexcept
{
    thread_local TimeoutTimerPool pool;
    return pool;
}

}  // namespace detail

class TimeoutTimer final: public Timer
{
public:
    using ptr = std::shared_ptr<TimeoutTimer>;

    /**
     * @brief 从调度线程本地对象池获取定时器，避免每操作堆分配。
     *
     * 正常创建与释放都发生在拥有该定时器的 IO 调度线程上；若 shared_ptr
     * 最后在外部线程释放，则直接析构而不进入外部线程的池。
     */
    static ptr create(std::chrono::milliseconds duration);

    /**
     * @brief 复用前清零上一轮使用的全部状态。
     *
     * 只有在最后一个 shared_ptr 已释放、且时间轮不再持有该 timer 时才能
     * 调用；此时 DeferredWaker 不会再收到完成通知，原地重建只负责释放上一轮
     * 可能仍持有的 TaskRef 并恢复初始状态。
     */
    void resetForReuse(std::chrono::milliseconds duration) noexcept
    {
        m_delay = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        m_expireTime = 0;
        m_flag.store(0, std::memory_order_relaxed);
        m_completion.store(Completion::kPending, std::memory_order_relaxed);
        // DeferredWaker 禁止移动赋值，原地重建恢复初始 kArming 状态。
        m_waker.~DeferredWaker();
        new (&m_waker) detail::DeferredWaker{};
    }

    /** @brief 破坏性异步操作尝试取得事务式完成权的结果。 */
    enum class OperationStart : uint8_t {
        kStarted,       ///< 当前调用方已取得完成权，timeout 将延迟裁决。
        kBusy,          ///< 另一操作正在持有事务式完成权。
        kTimeoutWon,    ///< timeout 已先完成，调用方不得修改 channel 数据。
        kOperationWon,  ///< 操作已完成，当前 waiter 是过期入口。
    };

    /** @brief 事务式操作未提交时释放完成权的结果。 */
    enum class OperationAbort : uint8_t {
        kRearmed,     ///< 未发生 timeout，完成权已恢复为 Pending。
        kTimeoutWon,  ///< timeout 曾在操作期间到达，调用方必须最后唤醒 timer waker。
        kCompleted,   ///< 完成权此前已经终结，无需再次唤醒。
    };

    template<concepts::ChronoDuration Duration>
    TimeoutTimer(Duration duration)
        : Timer(duration) {}

    void setWaker(Waker waker) noexcept { m_waker.setWaker(std::move(waker)); }

    /** @brief awaiter 恢复后释放 timer 持有的任务引用。 */
    void clearWaker() noexcept { m_waker.clearWaker(); }

    /**
     * @brief 发布外层 await_suspend 已完成全部注册访问。
     * @return true 表示应保持挂起；false 表示 timeout/operation 已提前完成，
     *         调用方应同步继续。
     */
    [[nodiscard]] bool armWaker() noexcept { return m_waker.arm(); }

    /** @brief 返回供 timeout-aware awaiter 共享的两阶段完成唤醒门。 */
    detail::DeferredWaker& completionWaker() noexcept { return m_waker; }

    /**
     * @brief 尝试由异步操作认领完成权并取消底层 timer。
     * @return true 表示操作已拥有完成权；false 表示 timeout 已获胜。
     */
    [[nodiscard]] bool tryCompleteOperation() noexcept
    {
        Completion expected = Completion::kPending;
        const bool operationWon = m_completion.compare_exchange_strong(
            expected,
            Completion::kOperationWon,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
        if (operationWon) {
            Timer::cancel();
        }
        return operationWon;
    }

    /**
     * @brief 在可能失败的 dequeue/enqueue 前取得事务式完成权。
     * @return kStarted 后调用方必须且只能调用一次 commitOperation() 或
     *         abortOperation()；其他结果表示不得开始破坏性数据操作。
     */
    [[nodiscard]] OperationStart tryBeginOperation() noexcept
    {
        Completion state = m_completion.load(std::memory_order_acquire);
        for (;;) {
            if (state == Completion::kPending) {
                if (m_completion.compare_exchange_weak(
                        state,
                        Completion::kOperationInFlight,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return OperationStart::kStarted;
                }
                continue;
            }
            if (state == Completion::kOperationInFlight ||
                state == Completion::kTimeoutRequested) {
                return OperationStart::kBusy;
            }
            if (state == Completion::kTimeoutWon) {
                return OperationStart::kTimeoutWon;
            }
            return OperationStart::kOperationWon;
        }
    }

    /**
     * @brief 提交已取得完成权的破坏性操作。
     * @return 成功把 InFlight/TimeoutRequested 提交为 OperationWon 时返回 true。
     * @note timeout 在操作期间到达时由先取得完成权的操作获胜，避免回滚已移动值。
     */
    [[nodiscard]] bool commitOperation() noexcept
    {
        Completion state = m_completion.load(std::memory_order_acquire);
        while (state == Completion::kOperationInFlight ||
               state == Completion::kTimeoutRequested) {
            if (m_completion.compare_exchange_weak(
                        state,
                        Completion::kOperationWon,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                Timer::cancel();
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 回滚未修改 channel 数据的事务式操作。
     * @return 未到达 timeout 时返回 kRearmed；timeout 已请求时返回 kTimeoutWon。
     * @note 返回 kTimeoutWon 的调用方必须先清理 waiter，再以 wakeTimeoutWinner()
     *       作为最后一次相关访问，避免恢复后访问已销毁协程帧。
     */
    [[nodiscard]] OperationAbort abortOperation() noexcept
    {
        Completion state = m_completion.load(std::memory_order_acquire);
        for (;;) {
            if (state == Completion::kOperationInFlight) {
                if (m_completion.compare_exchange_weak(
                        state,
                        Completion::kPending,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return OperationAbort::kRearmed;
                }
                continue;
            }
            if (state == Completion::kTimeoutRequested) {
                if (m_completion.compare_exchange_weak(
                        state,
                        Completion::kTimeoutWon,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return OperationAbort::kTimeoutWon;
                }
                continue;
            }
            return OperationAbort::kCompleted;
        }
    }

    /**
     * @brief 唤醒由 abortOperation() 最终裁决为 timeout 的任务。
     * @pre 当前调用方刚获得 OperationAbort::kTimeoutWon，且只调用一次。
     * @note wakeUp() 可能恢复并销毁 awaiter frame，因此必须是调用方最后一步。
     */
    void wakeTimeoutWinner()
    {
        if (!m_waker.requestWake()) {
            // operation/timeout 的另一完成路径已经发布同一唤醒。
        }
    }

    /** @brief 操作完成时取消 timeout 竞争；timer 已获胜时不会反转结果。 */
    void cancel() noexcept
    {
        if (!tryCompleteOperation()) {
            // producer 或 timeout 已认领完成权；仍确保底层 timer 停止后续调度。
            Timer::cancel();
        }
    }

    /** @brief timer 注册失败时同步标记超时并请求恢复已挂起任务。 */
    void timeoutNow()
    {
        if (completeTimeout()) {
            if (!m_waker.requestWake()) {
                // 完成已由另一条路径发布，无需重复唤醒。
            }
        }
    }

    /** @brief 在尚未发布 inner waiter 时只标记超时，不请求恢复。 */
    void markTimeoutWithoutWake()
    {
        if (!completeTimeout()) {
            // 另一完成方已先获胜，无需重复写入超时结果。
        }
    }

    bool timeouted() const noexcept {
        return m_completion.load(std::memory_order_acquire) == Completion::kTimeoutWon;
    }

    /** @brief 由 timer manager 或测试入口触发一次 timeout 裁决。 */
    void handleTimeout() override {
        if (completeTimeout()) {
            // wakeUp() 可能恢复并销毁 awaiter frame，因此必须是最后一次成员访问。
            if (!m_waker.requestWake()) {
                // 完成已由另一条路径发布，无需重复唤醒。
            }
        }
    }

private:
    TimeoutTimer(const TimeoutTimer&) = delete;
    TimeoutTimer& operator=(const TimeoutTimer&) = delete;

    enum class Completion : uint8_t {
        kPending,
        kOperationInFlight,
        kTimeoutRequested,
        kOperationWon,
        kTimeoutWon,
    };

    bool completeTimeout()
    {
        bool timeoutWon = false;
        Completion state = m_completion.load(std::memory_order_acquire);
        for (;;) {
            if (state == Completion::kPending) {
                if (m_completion.compare_exchange_weak(
                        state,
                        Completion::kTimeoutWon,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    timeoutWon = true;
                    break;
                }
                continue;
            }
            if (state == Completion::kOperationInFlight) {
                if (m_completion.compare_exchange_weak(
                        state,
                        Completion::kTimeoutRequested,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    break;
                }
                continue;
            }
            break;
        }
        Timer::handleTimeout();
        return timeoutWon;
    }

    detail::DeferredWaker m_waker;
    std::atomic<Completion> m_completion{Completion::kPending};
};

inline TimeoutTimer* detail::TimeoutTimerPool::acquire(std::chrono::milliseconds duration)
{
    if (!m_free.empty()) {
        auto* timer = m_free.back();
        m_free.pop_back();
        timer->resetForReuse(duration);
        return timer;
    }
    return new TimeoutTimer(duration);
}

inline void detail::TimeoutTimerPool::release(TimeoutTimer* timer) noexcept
{
    if (timer == nullptr) {
        return;
    }
    // 无抛出 deleter 可能在协程/frame 清理期间执行，不能让 vector 扩容把
    // 释放路径变成 terminate；空闲列表耗尽时直接销毁 timer。
    // 同时检查 capacity 与配置上限。构造函数会预留该上限，但 capacity
    // 检查可在不同翻译单元配置宏不一致时继续保证 noexcept 契约。
    if (m_free.size() < GALAY_KERNEL_TIMEOUT_TIMER_POOL_MAX_CACHED &&
        m_free.size() < m_free.capacity()) {
        m_free.push_back(timer);
    } else {
        delete timer;
    }
}

inline detail::TimeoutTimerPool::~TimeoutTimerPool()
{
    for (auto* timer : m_free) {
        delete timer;
    }
}

inline TimeoutTimer::ptr TimeoutTimer::create(std::chrono::milliseconds duration)
{
    struct PoolDeleter {
        std::thread::id owner;

        void operator()(TimeoutTimer* timer) const noexcept {
            if (std::this_thread::get_id() == owner) {
                detail::timeoutTimerPoolLocal().release(timer);
            } else {
                // 共享 timer 可能在其他线程晚于 scheduler frame 释放；不要
                // 把它发布到其他线程的本地对象池。
                delete timer;
            }
        }
    };
    return ptr(detail::timeoutTimerPoolLocal().acquire(duration),
               PoolDeleter{std::this_thread::get_id()});
}

template<typename Awaitable,
         typename TimeoutPolicyT = detail::DefaultTimeoutPolicy<Awaitable>>
struct WithTimeout;

/**
 * @brief 保存完成路径观察的超时定时器绑定。
 *
 * 普通 IO awaitable 和 sequence awaitable 共享这段完成时裁决逻辑；定时器
 * 的实际所有权仍由 WithTimeout 持有，绑定只保存非拥有观察指针。
 */
struct TimeoutTimerBinding {
    /**
     * @brief 绑定当前挂起操作对应的超时裁决定时器。
     *
     * WithTimeout 在 inner.await_suspend() 前调用；完成路径必须在唤醒协程
     * 前取消该定时器，把成功与超时的竞争裁决提前到完成派发时刻。组合
     * awaitable 可先暂存绑定，再在自己的 await_suspend() 中转交给 inner。
     */
    void bindTimeoutTimer(TimeoutTimer* timer) noexcept {
        m_bound_timeout_timer = timer;
    }

protected:
    /**
     * @brief 将尚未发布给完成方的绑定转交给实际 inner awaitable。
     *
     * 组合 awaitable 先在外层保存 timer，再在 inner.await_suspend() 前转交，
     * 从而不要求 inner 必须在外层 bindTimeoutTimer() 调用前完成构造。
     */
    template <typename AwaitableT>
    requires requires(AwaitableT& awaitable, TimeoutTimer* timer) {
        { awaitable.bindTimeoutTimer(timer) } noexcept;
    }
    void forwardBoundTimeoutTimer(AwaitableT& awaitable) noexcept {
        auto* timer = std::exchange(m_bound_timeout_timer, nullptr);
        if (timer != nullptr) {
            awaitable.bindTimeoutTimer(timer);
        }
    }

public:
    /** @brief 完成派发时刻取消竞争；幂等且不反转已发生的超时结果。 */
    void cancelBoundTimeoutTimer() noexcept {
        // reactor 和 await_resume() 都可能报告完成。取消前先清除非拥有观察
        // 指针，避免后续通知解引用已由 WithTimeout wrapper 释放的 timer。
        auto* timer = std::exchange(m_bound_timeout_timer, nullptr);
        if (timer != nullptr) {
            timer->cancel();
        }
    }

private:
    TimeoutTimer* m_bound_timeout_timer = nullptr;
};

/**
 * @brief 仅提供 `.timeout()` API 的 CRTP mixin。
 *
 * 与 TimeoutSupport 的区别是它不保存完成路径状态，供已经通过其他基类
 * 提供 TimeoutTimerBinding 的组合式 awaitable 使用。
 */
template<typename Derived,
         typename TimeoutPolicyT = detail::DefaultTimeoutPolicy<Derived>>
struct TimeoutMethods {
    using timeout_policy = TimeoutPolicyT;

    template<typename D = Derived>
    requires concepts::Awaitable<D>
    auto timeout(std::chrono::milliseconds t) && {
        return WithTimeout<Derived, TimeoutPolicyT>{
            std::move(static_cast<Derived&>(*this)), t};
    }

    template<typename D = Derived>
    requires concepts::Awaitable<D>
    auto timeout(std::chrono::milliseconds t) & {
        return WithTimeout<Derived, TimeoutPolicyT>{static_cast<Derived&>(*this), t};
    }
};

/**
* @brief CRTP 基类，为 Awaitable 提供 timeout() 方法
*/
template<typename Derived,
         typename TimeoutPolicyT = detail::DefaultTimeoutPolicy<Derived>>
struct TimeoutSupport
    : public TimeoutTimerBinding
    , public TimeoutMethods<Derived, TimeoutPolicyT> {
    using timeout_policy = TimeoutPolicyT;
};

/**
 * @brief 超时包装器
 *
 * @details 对于 io_uring，使用独立的 timeout 操作；对于 epoll/kqueue，使用 timerfd。
 * 定时器状态存储在 IOController 中，生命周期与 AsyncTcpSocket 绑定。
 * @note TimeoutTimer 在真正进入 await_suspend 前惰性创建；await_ready() 为真的路径
 *       不分配。Timer manager 继续通过 shared_ptr 管理跨线程取消生命周期，避免把
 *       awaiter frame 与时间轮节点耦合。超时结果由 TimeoutPolicyT 静态分发。
 */
template<typename Awaitable, typename TimeoutPolicyT>
struct WithTimeout {
    static_assert(
        std::same_as<TimeoutPolicyT, detail::DefaultTimeoutPolicy<Awaitable>>
            ? concepts::TimeoutInjectable<Awaitable>
            : (detail::TimeoutPolicy<TimeoutPolicyT, Awaitable> ||
               concepts::AlwaysReadyAwaitable<Awaitable>),
        "timeout policy must provide static inject(awaitable&), or the default "
        "policy requires markTimeout(), setTimeout(), m_result, or kAlwaysReady");

    std::chrono::milliseconds m_duration{};
    Awaitable m_inner;
    TimeoutTimer::ptr m_timer;
    Scheduler* m_scheduler = nullptr;

    WithTimeout(Awaitable&& inner, std::chrono::milliseconds timeout)
        : m_inner(std::move(inner)), m_duration(timeout) {}

    WithTimeout(Awaitable& inner, std::chrono::milliseconds timeout)
        : m_inner(std::move(inner)), m_duration(timeout) {}

    WithTimeout(WithTimeout&&) noexcept = default;
    WithTimeout& operator=(WithTimeout&&) noexcept = default;

    auto timeout(std::chrono::milliseconds t) && {
        return WithTimeout<Awaitable, TimeoutPolicyT>{std::move(m_inner), t};
    }

    auto timeout(std::chrono::milliseconds t) & {
        return WithTimeout<Awaitable, TimeoutPolicyT>{m_inner, t};
    }

    bool await_ready() { return m_inner.await_ready(); }

    template<typename Promise>
    requires concepts::AwaitableWith<Awaitable, Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        ensureTimer();
        auto timer = m_timer;
        auto waker = Waker(handle);
        Scheduler* scheduler = waker.getScheduler();
        m_scheduler = scheduler;
        timer->setWaker(std::move(waker));
        if (scheduler == nullptr) {
            timer->markTimeoutWithoutWake();
            injectTimeoutResult();
            return false;
        }

        if constexpr (requires(Awaitable& awaitable, TimeoutTimer* rawTimer) {
            awaitable.bindTimeoutTimer(rawTimer);
        }) {
            // 核心 IO awaitable 只观察共享完成 timer，不延长其生命周期；
            // WithTimeout 仍是唯一所有者。
            m_inner.bindTimeoutTimer(timer.get());
        } else if constexpr (requires(Awaitable& awaitable,
                                      const TimeoutTimer::ptr& sharedTimer) {
            awaitable.bindTimeoutTimer(sharedTimer);
        }) {
            // channel awaitable 保留同一个共享 timer 对象，使 waiter 与超时
            // 路径共同仲裁一个完成状态。
            m_inner.bindTimeoutTimer(timer);
        }

        const bool suspended = m_inner.await_suspend(handle);
        if (!suspended) {
            // 同步完成没有 reactor 派发回调；wrapper 销毁前仍需取消 timer，
            // CloseAwaitable 也会在此路径消费其裸绑定。
            timer->cancel();
            return false;
        }
        const bool timerAdded = scheduler->addTimer(timer);
        if (!timerAdded) {
            timer->timeoutNow();
        }
        // armWaker() 之后完成方可能立即恢复并销毁当前 awaiter，因此必须直接返回。
        return timer->armWaker();
    }

    auto await_resume() -> decltype(m_inner.await_resume()) {
        auto timer = std::move(m_timer);
        if (!timer) {
            return m_inner.await_resume();
        }
        const bool timedOut = timer->timeouted();
        // 当前协程已在执行，timer 不再需要保留独立 TaskRef。
        timer->clearWaker();
        if (timedOut) [[unlikely]] {
            const bool owns_registration = [&]() noexcept {
                if constexpr (requires(Awaitable& awaitable) {
                    { TimeoutPolicyT::ownsIoRegistration(awaitable) } -> std::convertible_to<bool>;
                }) {
                    return static_cast<bool>(TimeoutPolicyT::ownsIoRegistration(m_inner));
                } else {
                    return detail::awaitableStillOwnsIORegistration(m_inner);
                }
            }();
            if (!owns_registration) {
                timer->cancel();
                return m_inner.await_resume();
            }
            if constexpr (requires(Awaitable& awaitable) {
                awaitable.m_controller;
            }) {
                const bool removed_registration =
                    detail::removeTimedOutIORegistration(m_scheduler, m_inner.m_controller);
                (void)removed_registration;
            }
            injectTimeoutResult();
            timer->cancel();
        } else {
            // reactor 通常会在唤醒协程前取消绑定 timer。同步或自定义 awaitable
            // 的完成路径可能不经过 reactor，因此保留第二次取消；cancel()
            // 在这些路径之间刻意设计为幂等。
            timer->cancel();
        }
        return m_inner.await_resume();
    }

    /** @brief 显式为源码级测试或手工驱动 timer 的调用方创建定时器。 */
    void ensureTimer() {
        if (!m_timer) {
            m_timer = TimeoutTimer::create(m_duration);
        }
    }

    /** @brief 将嵌套 timeout 包装器的超时结果继续传给 inner。 */
    void markTimeout() { injectTimeoutResult(); }

private:
    void injectTimeoutResult()
    {
        if constexpr (std::same_as<TimeoutPolicyT, detail::DefaultTimeoutPolicy<Awaitable>>) {
            // This member is the friendship boundary retained by the low-level
            // channel awaiters.  New awaitables should use an explicit policy
            // and keep their result state private behind setTimeout().
            if constexpr (concepts::TimeoutMarkable<Awaitable>) {
                m_inner.markTimeout();
            } else if constexpr (concepts::TimeoutSettable<Awaitable>) {
                m_inner.setTimeout();
            } else if constexpr (requires(Awaitable& value) {
                value.m_result = std::unexpected(IOError(kTimeout, 0));
            }) {
                m_inner.m_result = std::unexpected(IOError(kTimeout, 0));
            } else {
                using result_type = std::remove_cvref_t<decltype(m_inner.await_resume())>;
                if constexpr (requires {
                    typename detail::timeout_expected_traits<result_type>::error_type;
                }) {
                    using error_type =
                        typename detail::timeout_expected_traits<result_type>::error_type;
                    if constexpr (std::is_constructible_v<error_type, IOError> &&
                                  requires(Awaitable& value) {
                                      value.m_result = std::unexpected(
                                          error_type(IOError(kTimeout, 0)));
                                  }) {
                        m_inner.m_result = std::unexpected(error_type(IOError(kTimeout, 0)));
                    } else if constexpr (!concepts::AlwaysReadyAwaitable<Awaitable>) {
                        static_assert(detail::timeout_always_false<Awaitable>,
                                      "default timeout policy has no injectable result");
                    }
                } else if constexpr (!concepts::AlwaysReadyAwaitable<Awaitable>) {
                    static_assert(detail::timeout_always_false<Awaitable>,
                                  "default timeout policy has no injectable result");
                }
            }
        } else {
            TimeoutPolicyT::inject(m_inner);
        }
    }

    WithTimeout(const WithTimeout&) = delete;
    WithTimeout& operator=(const WithTimeout&) = delete;
};

}



#endif
