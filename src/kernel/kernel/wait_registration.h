/**
 * @file wait_registration.h
 * @brief 原子等待/唤醒注册槽
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供 WaitRegistration，一种无锁单等待者注册机制，跟踪等待者地址
 * 和代数计数器。被 waker 系统用于协调跨线程的安全唤醒。
 */

#ifndef GALAY_KERNEL_WAIT_REGISTRATION_H
#define GALAY_KERNEL_WAIT_REGISTRATION_H

#include <atomic>
#include <cstdint>

namespace galay::kernel {

/**
 * @brief 无锁单等待者注册槽
 *
 * @details 允许一个等待者注册其地址，唤醒者可以原子地消费该地址。
 * 代数计数器用于检测等待者变更时的过期唤醒。
 */
class WaitRegistration
{
public:
    /**
     * @brief 在给定地址注册等待者
     *
     * @param waiter_address  标识等待实体的指针；不可为空
     * @return true 注册成功；false waiter_address 为空
     */
    bool arm(void* waiter_address) noexcept {
        if (!waiter_address) {
            return false;
        }
        m_waiter.store(waiter_address, std::memory_order_release);
        m_generation.fetch_add(1, std::memory_order_acq_rel);
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
    void* consumeWake() noexcept {
        return m_waiter.exchange(nullptr, std::memory_order_acq_rel);
    }

    /**
     * @brief 检查当前是否有等待者注册
     * @return true 存在等待者
     */
    bool hasWaiter() const noexcept {
        return m_waiter.load(std::memory_order_acquire) != nullptr;
    }

    /**
     * @brief 获取当前代数计数器
     * @return 单调递增的代数值
     */
    uint64_t generation() const noexcept {
        return m_generation.load(std::memory_order_acquire);
    }

private:
    std::atomic<void*> m_waiter{nullptr};  ///< 已注册的等待者地址
    std::atomic<uint64_t> m_generation{0};  ///< 每次 arm() 调用时递增
};

}  // namespace galay::kernel

#endif  // GALAY_KERNEL_WAIT_REGISTRATION_H
