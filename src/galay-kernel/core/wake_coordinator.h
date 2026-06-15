/**
 * @file wake_coordinator.h
 * @brief IO 调度器跨线程唤醒协调器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 管理任务注入线程与 IO 调度器事件循环之间的唤醒协议。
 * 合并冗余唤醒以减少系统调用，并跟踪休眠状态以判断是否需要唤醒。
 */

#ifndef GALAY_KERNEL_WAKE_COORDINATOR_H
#define GALAY_KERNEL_WAKE_COORDINATOR_H

#include <atomic>
#include <cstdint>
#include <utility>

namespace galay::kernel {

/**
 * @brief 协调注入线程与事件循环之间的唤醒请求
 *
 * @details 使用待处理标志和休眠标志来合并冗余唤醒。
 * 维护请求、发出和合并唤醒的诊断计数器。
 */
class WakeCoordinator
{
public:
    /**
     * @brief 构造绑定到调度器休眠/待处理标志的协调器
     *
     * @param sleeping       调度器休眠状态原子的引用
     * @param wakeup_pending 调度器待处理唤醒原子的引用
     */
    WakeCoordinator(std::atomic<bool>& sleeping, std::atomic<bool>& wakeup_pending) noexcept
        : m_sleeping(sleeping)
        , m_wakeup_pending(wakeup_pending)
    {
    }

    /**
     * @brief 请求唤醒，若调度器已处于唤醒状态则合并
     *
     * @tparam NotifyFn  执行实际唤醒的可调用对象（如 eventfd 写入）
     * @param queue_was_empty  本次入队前注入队列是否为空
     * @param notify_fn        未合并时调用的唤醒函数
     * @return true 唤醒已实际发出；false 已合并或已有待处理唤醒
     */
    template <typename NotifyFn>
    bool requestWake(bool queue_was_empty, NotifyFn&& notify_fn) {
        m_wake_requests.fetch_add(1, std::memory_order_relaxed);
        if (!(queue_was_empty || m_sleeping.load(std::memory_order_acquire))) {
            m_coalesced_wakes.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (m_wakeup_pending.exchange(true, std::memory_order_acq_rel)) {
            m_coalesced_wakes.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::forward<NotifyFn>(notify_fn)();
        m_wake_emits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief 无条件发出唤醒（用于关闭期间）
     *
     * @tparam NotifyFn  执行实际唤醒的可调用对象
     * @param notify_fn  唤醒函数
     */
    template <typename NotifyFn>
    void forceWake(NotifyFn&& notify_fn) {
        std::forward<NotifyFn>(notify_fn)();
        m_wake_emits.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief 标记事件循环进入可能的休眠状态
     */
    void markSleeping() noexcept {
        m_sleeping.store(true, std::memory_order_release);
    }

    /**
     * @brief 标记事件循环为唤醒状态（poll 或任务处理之后）
     */
    void markAwake() noexcept {
        m_sleeping.store(false, std::memory_order_release);
    }

    /**
     * @brief 清除待处理唤醒标志（消费唤醒事件后调用）
     */
    void cancelPendingWake() noexcept {
        m_wakeup_pending.store(false, std::memory_order_release);
    }

    /**
     * @brief 处理远程任务排空完成
     *
     * @param drained  从注入队列排空的任务数
     * @details 若有任务被排空，标记循环为唤醒状态并取消待处理唤醒，
     *          因为循环已在运行。
     */
    void onRemoteCollected(size_t drained) noexcept {
        if (drained == 0) {
            return;
        }
        markAwake();
        cancelPendingWake();
    }

    /** @return true 事件循环当前处于休眠状态 */
    bool isSleeping() const noexcept {
        return m_sleeping.load(std::memory_order_acquire);
    }

    /** @return true 唤醒已发出但尚未被消费 */
    bool hasPendingWake() const noexcept {
        return m_wakeup_pending.load(std::memory_order_acquire);
    }

    /** @return 收到的唤醒请求总数 */
    uint64_t wakeRequests() const noexcept {
        return m_wake_requests.load(std::memory_order_acquire);
    }

    /** @return 实际发出的唤醒系统调用总数 */
    uint64_t wakeEmits() const noexcept {
        return m_wake_emits.load(std::memory_order_acquire);
    }

    /** @return 被合并（跳过）的唤醒总数 */
    uint64_t coalescedWakes() const noexcept {
        return m_coalesced_wakes.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool>& m_sleeping;        ///< 调度器休眠状态的外部引用
    std::atomic<bool>& m_wakeup_pending;  ///< 待处理唤醒标志的外部引用
    std::atomic<uint64_t> m_wake_requests{0};   ///< 诊断：请求总数
    std::atomic<uint64_t> m_wake_emits{0};      ///< 诊断：发出总数
    std::atomic<uint64_t> m_coalesced_wakes{0}; ///< 诊断：合并总数
};

}  // namespace galay::kernel

#endif  // GALAY_KERNEL_WAKE_COORDINATOR_H
