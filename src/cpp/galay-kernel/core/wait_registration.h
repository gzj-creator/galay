/**
 * @file wait_registration.h
 * @brief 原子等待/唤醒注册槽
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供 WaitRegistration，一种无锁单等待者注册机制，跟踪等待者地址。
 * 被 waker 系统用于协调跨线程的安全唤醒。
 */

#ifndef GALAY_KERNEL_WAIT_REGISTRATION_H
#define GALAY_KERNEL_WAIT_REGISTRATION_H

#include <atomic>

namespace galay::kernel {

/**
 * @brief 无锁单等待者注册槽
 *
 * @details 允许一个等待者注册其地址，唤醒者可以原子地消费该地址。
 */
class WaitRegistration
{
public:
    /**
     * @brief 在给定地址注册等待者
     *
     * @param waiter_address  标识等待实体的指针；不可为空
     * @return true 注册成功；false 地址为空或槽位仍被前一个 waiter 占用
     */
    bool arm(void* waiter_address) noexcept {
        if (!waiter_address) {
            return false;
        }
        void* expected = nullptr;
        if (!m_waiter.compare_exchange_strong(expected,
                                              waiter_address,
                                              std::memory_order_release,
                                              std::memory_order_relaxed)) {
            return false;
        }
        return true;
    }

    /**
     * @brief 移除指定等待者的注册
     *
     * @param waiter_address  期望清除的等待者地址
     * @return true 成功清除；false 地址不匹配或已为空
     */
    bool clear(void* waiter_address) noexcept {
        return waiter_address &&
               m_waiter.compare_exchange_strong(waiter_address,
                                               nullptr,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire);
    }

    /**
     * @brief 原子地消费当前等待者并返回
     *
     * @return 之前的等待者地址，若无注册则返回 nullptr
     */
    void* consume_wake() noexcept {
        void* waiter = m_waiter.exchange(nullptr, std::memory_order_acq_rel);
        if (waiter == nullptr) {
            m_pending_wake.store(true, std::memory_order_release);
        }
        return waiter;
    }

    /**
     * @brief 消费一次在 waiter 注册前到达的唤醒。
     *
     * @return true 表示有一次唤醒早于 arm() 到达，调用方应自行调度刚注册的等待者。
     */
    bool consume_pending_wake() noexcept {
        return m_pending_wake.exchange(false, std::memory_order_acq_rel);
    }

    /**
     * @brief 清理过期的提前唤醒标记。
     * @return 无返回值
     * @note 当等待者通过 await_ready()/try_recv() 等同步路径直接拿到数据时调用。
     */
    void clear_pending_wake() noexcept {
        m_pending_wake.store(false, std::memory_order_release);
    }

    /**
     * @brief 检查当前是否有等待者注册
     * @return true 存在等待者
     */
    bool has_waiter() const noexcept {
        return m_waiter.load(std::memory_order_acquire) != nullptr;
    }

private:
    std::atomic<void*> m_waiter{nullptr};  ///< 已注册的等待者地址
    std::atomic<bool> m_pending_wake{false};  ///< waiter 注册前到达的一次唤醒
};

}  // namespace galay::kernel

#endif  // GALAY_KERNEL_WAIT_REGISTRATION_H
