/**
 * @file safetimer_mgr.hpp
 * @brief 线程安全的多层时间轮定时器管理器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 设计灵感来自 Tokio 的定时器实现：
 * - 添加路径：无锁 MPSC 队列（ConcurrentQueue）接收新定时器
 * - 处理路径：单线程批量出队和时间轮操作
 * - 取消：每个 Timer 上的原子标志
 *
 * 性能特征：
 * - 添加定时器：O(1)，无锁
 * - 取消定时器：O(1)，原子操作
 * - tick 处理：O(到期定时器数量)
 */

#ifndef GALAY_KERNEL_THREAD_SAFE_TIMER_MANAGER_HPP
#define GALAY_KERNEL_THREAD_SAFE_TIMER_MANAGER_HPP

#include "timer.hpp"
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>
#include <chrono>
#include <atomic>

namespace galay::kernel
{

/**
 * @brief 线程安全的多层时间轮定时器管理器
 *
 * 使用无锁 MPSC 队列接收定时器，单线程处理时间轮。
 *
 * 线程安全性：
 * - push(): 多线程安全，可从任意线程调用
 * - tick(): 单线程调用，由定时器线程驱动
 * - size()/empty(): 近似值，用于监控
 *
 * 时间轮结构（默认 1ms tick）：
 * - 第1层：256个槽，每槽 1ms，覆盖 0-255ms
 * - 第2层：64个槽，每槽 256ms，覆盖 256ms-16s
 * - 第3层：64个槽，每槽 16s，覆盖 16s-17分钟
 * - 第4层：64个槽，每槽 17分钟，覆盖 17分钟-18小时
 * - 第5层：64个槽，每槽 18小时，覆盖 18小时-48天
 */
class ThreadSafeTimerManager
{
public:
    using ptr = std::shared_ptr<ThreadSafeTimerManager>;
    using TimerList = std::list<Timer::ptr>;

    // 时间轮配置
    static constexpr size_t WHEEL1_SIZE = 256;
    static constexpr size_t WHEEL2_SIZE = 64;
    static constexpr size_t WHEEL3_SIZE = 64;
    static constexpr size_t WHEEL4_SIZE = 64;
    static constexpr size_t WHEEL5_SIZE = 64;

    // 每层的时间跨度（以 tick 为单位）
    static constexpr uint64_t WHEEL1_SPAN = WHEEL1_SIZE;
    static constexpr uint64_t WHEEL2_SPAN = WHEEL1_SPAN * WHEEL2_SIZE;
    static constexpr uint64_t WHEEL3_SPAN = WHEEL2_SPAN * WHEEL3_SIZE;
    static constexpr uint64_t WHEEL4_SPAN = WHEEL3_SPAN * WHEEL4_SIZE;
    static constexpr uint64_t WHEEL5_SPAN = WHEEL4_SPAN * WHEEL5_SIZE;

    // 批量处理大小
    static constexpr size_t BATCH_SIZE = 256;

    /**
     * @brief 构造函数
     * @param tickDuration 每个 tick 的时间间隔（纳秒）
     */
    explicit ThreadSafeTimerManager(uint64_t tickDuration = 1000000ULL)
        : m_wheelSize(0)
        , m_pendingSize(0)
        , m_tickDuration(tickDuration)
        , m_currentTick(0)
        , m_wheel1(WHEEL1_SIZE)
        , m_wheel2(WHEEL2_SIZE)
        , m_wheel3(WHEEL3_SIZE)
        , m_wheel4(WHEEL4_SIZE)
        , m_wheel5(WHEEL5_SIZE)
        , m_startTime(std::chrono::steady_clock::now())
    {
    }

    /**
     * @brief 添加定时器（线程安全，无锁）
     *
     * 可从任意线程调用，定时器会被放入待处理队列，
     * 在下次 tick() 时被处理。
     *
     * @param timer 定时器共享指针
     * @return true 添加成功
     */
    bool push(Timer::ptr timer)
    {
        if (!timer) {
            return false;
        }

        // 触发过期时间计算（延迟计算）
        timer->getExpireTime();

        // 无锁入队
        if (m_pendingQueue.enqueue(std::move(timer))) {
            m_pendingSize.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    /**
     * @brief 批量添加定时器（线程安全，无锁）
     * @param timers 定时器列表
     * @return 成功添加的数量
     */
    size_t pushBatch(const std::vector<Timer::ptr>& timers)
    {
        size_t count = 0;
        for (const auto& timer : timers) {
            if (timer) {
                timer->getExpireTime();
                if (m_pendingQueue.enqueue(timer)) {
                    ++count;
                }
            }
        }
        m_pendingSize.fetch_add(count, std::memory_order_relaxed);
        return count;
    }

    /**
     * @brief 检查是否为空（近似值）
     */
    bool empty() const
    {
        return m_wheelSize.load(std::memory_order_relaxed) == 0 &&
               m_pendingSize.load(std::memory_order_relaxed) == 0;
    }

    /**
     * @brief 获取定时器总数（近似值）
     */
    size_t size() const
    {
        return m_wheelSize.load(std::memory_order_relaxed) +
               m_pendingSize.load(std::memory_order_relaxed);
    }

    /**
     * @brief 获取时间轮中的定时器数量
     */
    size_t wheelSize() const
    {
        return m_wheelSize.load(std::memory_order_relaxed);
    }

    /**
     * @brief 获取待处理队列中的定时器数量
     */
    size_t pendingSize() const
    {
        return m_pendingSize.load(std::memory_order_relaxed);
    }

    /**
     * @brief 获取 tick 间隔（纳秒）
     */
    uint64_t during() const
    {
        return m_tickDuration;
    }

    /**
     * @brief 清空所有待处理和已入轮的定时器
     * @details 应在没有 tick()/push() 并发访问时调用，典型场景是调度器线程停止后重置全局定时器。
     */
    void clear()
    {
        Timer::ptr timer;
        while (m_pendingQueue.try_dequeue(timer)) {
        }

        for (auto* wheel : {&m_wheel1, &m_wheel2, &m_wheel3, &m_wheel4, &m_wheel5}) {
            for (auto& slot : *wheel) {
                slot.clear();
            }
        }

        m_pendingSize.store(0, std::memory_order_relaxed);
        m_wheelSize.store(0, std::memory_order_relaxed);
        reset();
    }

    /**
     * @brief 推进时间轮（单线程调用）
     *
     * 该方法会：
     * 1. 批量处理待添加的定时器
     * 2. 推进时间轮到当前时间
     * 3. 执行到期的定时器回调
     *
     * @note 只能由定时器线程调用
     */
    void tick()
    {
        // 1. 处理待添加的定时器
        processPendingTimers();

        // 2. 快速路径：如果没有定时器，直接返回
        if (m_wheelSize.load(std::memory_order_relaxed) == 0) {
            return;
        }

        // 3. 计算目标 tick
        auto now = std::chrono::steady_clock::now();
        uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - m_startTime).count();
        uint64_t targetTick = elapsed / m_tickDuration;

        // 快速路径：如果还没到下一个 tick，直接返回
        if (targetTick <= m_currentTick) {
            return;
        }

        // 4. 处理所有应该执行的 tick
        while (m_currentTick < targetTick &&
               m_wheelSize.load(std::memory_order_relaxed) > 0) {

            // 级联处理
            if ((m_currentTick & (WHEEL1_SIZE - 1)) == 0 && m_currentTick > 0) {
                cascadeWheel2();
            }
            if ((m_currentTick & (WHEEL2_SPAN - 1)) == 0 && m_currentTick > 0) {
                cascadeWheel3();
            }
            if ((m_currentTick & (WHEEL3_SPAN - 1)) == 0 && m_currentTick > 0) {
                cascadeWheel4();
            }
            if ((m_currentTick & (WHEEL4_SPAN - 1)) == 0 && m_currentTick > 0) {
                cascadeWheel5();
            }

            // 处理第1层当前槽
            processWheel1();

            ++m_currentTick;
        }

        // 5. 当所有定时器都处理完时自动重置
        if (m_wheelSize.load(std::memory_order_relaxed) == 0) {
            reset();
        }
    }

    ~ThreadSafeTimerManager() = default;

private:
    /**
     * @brief 批量处理待添加的定时器
     */
    void processPendingTimers()
    {
        Timer::ptr timers[BATCH_SIZE];
        size_t count;

        // 批量出队处理
        while ((count = m_pendingQueue.try_dequeue_bulk(timers, BATCH_SIZE)) > 0) {
            m_pendingSize.fetch_sub(count, std::memory_order_relaxed);

            const auto now = std::chrono::steady_clock::now();
            const uint64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count();
            const uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - m_startTime).count();
            const uint64_t currentTickFromStart = elapsed / m_tickDuration;

            for (size_t i = 0; i < count; ++i) {
                addTimerToWheel(std::move(timers[i]), nowNs, currentTickFromStart);
            }
        }
    }

    /**
     * @brief 将定时器添加到时间轮的合适位置
     */
    void addTimerToWheel(Timer::ptr timer, uint64_t nowNs, uint64_t currentTickFromStart)
    {
        if (!timer || timer->done() || timer->cancelled()) {
            return;
        }

        // 获取定时器的绝对过期时间（纳秒）
        uint64_t expireTimeNs = timer->getExpireTime();

        // 计算剩余时间（纳秒）
        if (expireTimeNs <= nowNs) {
            // 已经过期，立即执行
            timer->handleTimeout();
            return;
        }

        uint64_t remainingNs = expireTimeNs - nowNs;
        uint64_t delayTicks = remainingNs / m_tickDuration;

        if (delayTicks == 0) {
            // 不足一个 tick，立即执行
            timer->handleTimeout();
            return;
        }

        // 检查是否超出最大范围
        if (delayTicks >= WHEEL5_SPAN) {
            // 超出范围，丢弃
            return;
        }

        // 计算绝对到期 tick
        uint64_t absoluteTick = currentTickFromStart + delayTicks;

        // 根据延迟 tick 数选择合适的层
        if (delayTicks < WHEEL1_SPAN) {
            size_t slot = absoluteTick & (WHEEL1_SIZE - 1);
            m_wheel1[slot].push_back(std::move(timer));
        } else if (delayTicks < WHEEL2_SPAN) {
            size_t slot = (absoluteTick >> 8) & (WHEEL2_SIZE - 1);
            m_wheel2[slot].push_back(std::move(timer));
        } else if (delayTicks < WHEEL3_SPAN) {
            size_t slot = (absoluteTick >> 14) & (WHEEL3_SIZE - 1);
            m_wheel3[slot].push_back(std::move(timer));
        } else if (delayTicks < WHEEL4_SPAN) {
            size_t slot = (absoluteTick >> 20) & (WHEEL4_SIZE - 1);
            m_wheel4[slot].push_back(std::move(timer));
        } else {
            size_t slot = (absoluteTick >> 26) & (WHEEL5_SIZE - 1);
            m_wheel5[slot].push_back(std::move(timer));
        }

        m_wheelSize.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief 处理第1层当前槽的定时器
     */
    void processWheel1()
    {
        size_t idx = m_currentTick & (WHEEL1_SIZE - 1);
        auto& slot = m_wheel1[idx];

        for (auto& timer : slot) {
            if (!timer->done() && !timer->cancelled()) {
                timer->handleTimeout();
            }
            m_wheelSize.fetch_sub(1, std::memory_order_relaxed);
        }
        slot.clear();
    }

    /**
     * @brief 将第2层当前槽的定时器降级到第1层
     */
    void cascadeWheel2()
    {
        size_t idx = (m_currentTick >> 8) & (WHEEL2_SIZE - 1);
        cascadeSlot(m_wheel2[idx]);
    }

    void cascadeWheel3()
    {
        size_t idx = (m_currentTick >> 14) & (WHEEL3_SIZE - 1);
        cascadeSlot(m_wheel3[idx]);
    }

    void cascadeWheel4()
    {
        size_t idx = (m_currentTick >> 20) & (WHEEL4_SIZE - 1);
        cascadeSlot(m_wheel4[idx]);
    }

    void cascadeWheel5()
    {
        size_t idx = (m_currentTick >> 26) & (WHEEL5_SIZE - 1);
        cascadeSlot(m_wheel5[idx]);
    }

    /**
     * @brief 将指定槽的定时器重新分配到合适的层
     */
    void cascadeSlot(TimerList& slot)
    {
        TimerList temp = std::move(slot);
        slot.clear();

        auto now = std::chrono::steady_clock::now();
        uint64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();

        for (auto& timer : temp) {
            if (timer->done() || timer->cancelled()) {
                m_wheelSize.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }

            uint64_t expireTimeNs = timer->getExpireTime();

            if (expireTimeNs <= nowNs) {
                timer->handleTimeout();
                m_wheelSize.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }

            uint64_t remainingNs = expireTimeNs - nowNs;
            uint64_t remainingTicks = remainingNs / m_tickDuration;

            if (remainingTicks == 0) {
                timer->handleTimeout();
                m_wheelSize.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }

            uint64_t absoluteTick = m_currentTick + remainingTicks;

            // 重新分配到合适的层（不增加 wheelSize，因为已经在轮中）
            if (remainingTicks < WHEEL1_SPAN) {
                size_t slot_idx = absoluteTick & (WHEEL1_SIZE - 1);
                m_wheel1[slot_idx].push_back(std::move(timer));
            } else if (remainingTicks < WHEEL2_SPAN) {
                size_t slot_idx = (absoluteTick >> 8) & (WHEEL2_SIZE - 1);
                m_wheel2[slot_idx].push_back(std::move(timer));
            } else if (remainingTicks < WHEEL3_SPAN) {
                size_t slot_idx = (absoluteTick >> 14) & (WHEEL3_SIZE - 1);
                m_wheel3[slot_idx].push_back(std::move(timer));
            } else if (remainingTicks < WHEEL4_SPAN) {
                size_t slot_idx = (absoluteTick >> 20) & (WHEEL4_SIZE - 1);
                m_wheel4[slot_idx].push_back(std::move(timer));
            } else if (remainingTicks < WHEEL5_SPAN) {
                size_t slot_idx = (absoluteTick >> 26) & (WHEEL5_SIZE - 1);
                m_wheel5[slot_idx].push_back(std::move(timer));
            } else {
                // 超出范围，丢弃
                m_wheelSize.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }

    /**
     * @brief 重置时间轮
     */
    void reset()
    {
        m_startTime = std::chrono::steady_clock::now();
        m_currentTick = 0;
    }

private:
    // 计数器（原子操作，用于监控）
    alignas(64) std::atomic<size_t> m_wheelSize;
    alignas(64) std::atomic<size_t> m_pendingSize;

    uint64_t m_tickDuration;
    uint64_t m_currentTick;

    // 时间轮（只由定时器线程访问，无需加锁）
    std::vector<TimerList> m_wheel1;
    std::vector<TimerList> m_wheel2;
    std::vector<TimerList> m_wheel3;
    std::vector<TimerList> m_wheel4;
    std::vector<TimerList> m_wheel5;

    // 待处理队列（无锁 MPSC）
    moodycamel::ConcurrentQueue<Timer::ptr> m_pendingQueue;

    std::chrono::steady_clock::time_point m_startTime;
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_THREAD_SAFE_TIMER_MANAGER_HPP
