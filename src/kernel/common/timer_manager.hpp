/**
 * @file timer_manager.hpp
 * @brief 非线程安全的多层时间轮定时器管理器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现受时钟机制启发的五层时间轮（时/分/秒类比）。
 * 插入和取消操作为 O(1)。单线程：所有公共方法必须从同一线程调用。
 *
 * 默认 tick 粒度为 50 毫秒，覆盖范围约 6.8 年。
 * tick 时长可在构造时配置。
 *
 * @note 多线程场景请使用 ThreadSafeTimerManager（safetimer_mgr.hpp）。
 */

#ifndef GALAY_KERNEL_TIMERMANAGER_HPP
#define GALAY_KERNEL_TIMERMANAGER_HPP

#include "timer.hpp"
#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>
#include <chrono>

namespace galay::kernel
{

    // 非线程安全的多层时间轮定时器管理器
    // 使用分层时间轮结构，类似时钟的时分秒设计，支持超大范围的定时器
    // 插入/删除时间复杂度 O(1)
    //
    // 时间轮结构（默认 1ms tick）：
    // - 第1层：256个槽，每槽 1ms，覆盖 0-255ms
    // - 第2层：64个槽，每槽 256ms，覆盖 256ms-16s
    // - 第3层：64个槽，每槽 16s，覆盖 16s-17分钟
    // - 第4层：64个槽，每槽 17分钟，覆盖 17分钟-18小时
    // - 第5层：64个槽，每槽 18小时，覆盖 18小时-48天
    // 总覆盖范围：约 48 天（1ms tick）
    /**
     * @brief 非线程安全的多层时间轮定时器管理器
     *
     * @details 实现类似钟面的五层时间轮。插入和取消操作为 O(1)。
     * 单线程：所有公共方法必须从同一线程调用。
     *
     * 默认 tick 粒度为 50 毫秒，覆盖约 6.8 年。
     * tick 时长可在构造时配置。
     *
     * @note 多线程场景请使用 ThreadSafeTimerManager（safetimer_mgr.hpp）。
     */
    class TimingWheelTimerManager
    {
    public:
        using ptr = std::shared_ptr<TimingWheelTimerManager>;
        using TimerList = std::list<Timer::ptr>;

        // 时间轮配置
        static constexpr size_t WHEEL1_SIZE = 256;  ///< 第1层：256 个槽
        static constexpr size_t WHEEL2_SIZE = 64;   ///< 第2层：64 个槽
        static constexpr size_t WHEEL3_SIZE = 64;   ///< 第3层：64 个槽
        static constexpr size_t WHEEL4_SIZE = 64;   ///< 第4层：64 个槽
        static constexpr size_t WHEEL5_SIZE = 64;   ///< 第5层：64 个槽

        // 每层的时间跨度（以 tick 为单位）
        static constexpr uint64_t WHEEL1_SPAN = WHEEL1_SIZE;                           ///< 256 ticks
        static constexpr uint64_t WHEEL2_SPAN = WHEEL1_SPAN * WHEEL2_SIZE;             ///< 16384 ticks
        static constexpr uint64_t WHEEL3_SPAN = WHEEL2_SPAN * WHEEL3_SIZE;             ///< 1048576 ticks
        static constexpr uint64_t WHEEL4_SPAN = WHEEL3_SPAN * WHEEL4_SIZE;             ///< 67108864 ticks
        static constexpr uint64_t WHEEL5_SPAN = WHEEL4_SPAN * WHEEL5_SIZE;             ///< 4294967296 ticks

        /**
         * @brief 以可配置的 tick 间隔构造时间轮管理器
         *
         * @param tickDuration 每 tick 的纳秒数；控制定时器精度和最大范围
         *
         * @details 常见配置：
         * - 1 ms  (1e6)：覆盖 0 -- 48 天
         * - 10 ms (1e7)：覆盖 0 -- 497 天
         * - 50 ms (5e7)：覆盖 0 -- 6.8 年（默认）
         * - 100 ms (1e8)：覆盖 0 -- 13 年
         * - 1 s   (1e9)：覆盖 0 -- 136 年
         */
        explicit TimingWheelTimerManager(uint64_t tickDuration = 50000000ULL)
            : m_tickDuration(tickDuration)
            , m_currentTick(0)
            , m_wheel1(WHEEL1_SIZE)
            , m_wheel2(WHEEL2_SIZE)
            , m_wheel3(WHEEL3_SIZE)
            , m_wheel4(WHEEL4_SIZE)
            , m_wheel5(WHEEL5_SIZE)
            , m_size(0)
            , m_startTime(std::chrono::steady_clock::now())
        {
        }

        /**
         * @brief 向时间轮添加定时器
         * @param timer Timer 的共享指针
         * @return 若已添加（或已过期并执行）则返回 true，若超出范围或为空则返回 false
         *
         * @details 若定时器已过期，立即调用 handleTimeout() 并返回 true。
         * 若时间轮为空，重置起始时间以避免过期的 tick 推进。
         */
        bool push(Timer::ptr timer)
        {
            if (!timer) {
                return false;
            }

            // 获取定时器的绝对过期时间（纳秒）
            uint64_t expireTimeNs = timer->getExpireTime();

            // 获取当前时间（纳秒）
            auto now = std::chrono::steady_clock::now();
            uint64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count();

            // 空闲时间轮重新以当前时刻作为基准，避免首个新定时器被历史 tick 立即冲刷。
            if (m_size == 0) {
                m_startTime = now;
                m_currentTick = 0;
            }

            // 计算剩余时间（纳秒）
            if (expireTimeNs <= nowNs) {
                // 已经过期，立即执行
                timer->handleTimeout();
                return true;
            }

            uint64_t remainingNs = expireTimeNs - nowNs;

            // 向上取整到下一个 tick，避免边界定时器因少量调度开销被立即触发。
            uint64_t delayTicks = (remainingNs + m_tickDuration - 1) / m_tickDuration;

            // 计算绝对到期 tick（使用与 tick() 相同的计算方式）
            uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - m_startTime).count();
            uint64_t currentTickFromStart = elapsed / m_tickDuration;
            uint64_t absoluteTick = currentTickFromStart + delayTicks;
            // 检查是否超出最大范围
            if (delayTicks >= WHEEL5_SPAN) {
                return false;  // 超出范围
            }

            // 根据延迟 tick 数选择合适的层
            if (delayTicks < WHEEL1_SPAN) {
                // 第1层：0-255 ticks
                size_t slot = absoluteTick & (WHEEL1_SIZE - 1);
                m_wheel1[slot].push_back(timer);
            } else if (delayTicks < WHEEL2_SPAN) {
                // 第2层：256-16383 ticks
                size_t slot = (absoluteTick >> 8) & (WHEEL2_SIZE - 1);
                m_wheel2[slot].push_back(timer);
            } else if (delayTicks < WHEEL3_SPAN) {
                // 第3层：16384-1048575 ticks
                size_t slot = (absoluteTick >> 14) & (WHEEL3_SIZE - 1);
                m_wheel3[slot].push_back(timer);
            } else if (delayTicks < WHEEL4_SPAN) {
                // 第4层：1048576-67108863 ticks
                size_t slot = (absoluteTick >> 20) & (WHEEL4_SIZE - 1);
                m_wheel4[slot].push_back(timer);
            } else {
                // 第5层：67108864-4294967295 ticks
                size_t slot = (absoluteTick >> 26) & (WHEEL5_SIZE - 1);
                m_wheel5[slot].push_back(timer);
            }

            ++m_size;

            return true;
        }

        /**
         * @brief 检查时间轮是否包含任何活跃定时器
         * @return 若没有待处理的定时器则返回 true
         */
        bool empty() const
        {
            return m_size == 0;
        }

        /**
         * @brief 获取活跃定时器数量
         * @return 定时器数量
         */
        size_t size() const
        {
            return m_size;
        }

        /**
         * @brief 获取配置的 tick 时长（纳秒）
         * @return 每 tick 的纳秒数
         */
        uint64_t during() {
            return m_tickDuration;
        }

        /**
         * @brief 推进时间轮并触发过期定时器
         *
         * @details 从当前挂钟时间计算目标 tick，
         * 然后从当前 tick 迭代到目标 tick。每一步：
         * 1. 当跨越层边界时，将高层槽级联下降
         * 2. 触发第1层当前槽中的所有定时器
         *
         * @note 必须仅从单线程调用。
         */
        void tick()
        {
            // 快速路径：如果没有定时器，直接返回
            if (m_size == 0) {
                return;
            }

            auto now = std::chrono::steady_clock::now();
            uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - m_startTime).count();
            uint64_t targetTick = elapsed / m_tickDuration;

            // 快速路径：如果还没到下一个 tick，直接返回
            if (targetTick <= m_currentTick) {
                return;
            }

            // 处理所有应该执行的 tick
            while (m_currentTick <= targetTick && m_size > 0) {
                // 级联必须在处理当前槽之前执行
                // 级联：每 256 tick，从第2层降级到第1层
                if ((m_currentTick & (WHEEL1_SIZE - 1)) == 0 && m_currentTick > 0) {
                    cascadeWheel2();
                }

                // 级联：每 16384 tick，从第3层降级到第2层
                if ((m_currentTick & (WHEEL2_SPAN - 1)) == 0 && m_currentTick > 0) {
                    cascadeWheel3();
                }

                // 级联：每 1048576 tick，从第4层降级到第3层
                if ((m_currentTick & (WHEEL3_SPAN - 1)) == 0 && m_currentTick > 0) {
                    cascadeWheel4();
                }

                // 级联：每 67108864 tick，从第5层降级到第4层
                if ((m_currentTick & (WHEEL4_SPAN - 1)) == 0 && m_currentTick > 0) {
                    cascadeWheel5();
                }

                // 处理第1层当前槽
                processWheel1();

                ++m_currentTick;
            }

            // 当所有定时器都处理完时自动重置，防止 tick 溢出
            if (m_size == 0) {
                reset();
            }
        }

        ~TimingWheelTimerManager() = default;

    private:
        /**
         * @brief 触发第1层当前槽中的所有定时器
         */
        void processWheel1()
        {
            size_t idx = m_currentTick & (WHEEL1_SIZE - 1);
            auto& slot = m_wheel1[idx];

            auto it = slot.begin();
            while (it != slot.end()) {
                auto timer = *it;

                // 第1层的定时器都应该到期，直接执行
                timer->handleTimeout();

                it = slot.erase(it);
                --m_size;
            }
        }

        /**
         * @brief 将第2层定时器级联下降到第1层
         */
        void cascadeWheel2()
        {
            size_t idx = (m_currentTick >> 8) & (WHEEL2_SIZE - 1);
            cascadeSlot(m_wheel2[idx]);
        }

        /**
         * @brief 将第3层定时器级联下降到低层
         */
        void cascadeWheel3()
        {
            size_t idx = (m_currentTick >> 14) & (WHEEL3_SIZE - 1);
            cascadeSlot(m_wheel3[idx]);
        }

        /**
         * @brief 将第4层定时器级联下降到低层
         */
        void cascadeWheel4()
        {
            size_t idx = (m_currentTick >> 20) & (WHEEL4_SIZE - 1);
            cascadeSlot(m_wheel4[idx]);
        }

        /**
         * @brief 将第5层定时器级联下降到低层
         */
        void cascadeWheel5()
        {
            size_t idx = (m_currentTick >> 26) & (WHEEL5_SIZE - 1);
            cascadeSlot(m_wheel5[idx]);
        }

        /**
         * @brief 将高层槽中的定时器重新分配到正确的低层
         * @param slot 正在级联的定时器列表
         *
         * @details 每个定时器被重新评估：若已过期则立即触发；
         * 否则插入到合适的时间轮层。
         */
        void cascadeSlot(TimerList& slot)
        {
            TimerList temp = std::move(slot);
            slot.clear();

            // 获取当前时间（纳秒）
            auto now = std::chrono::steady_clock::now();
            uint64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count();

            for (auto& timer : temp) {
                if (timer->done() || timer->cancelled()) {
                    --m_size;
                    continue;
                }

                // 获取定时器的绝对过期时间（纳秒）
                uint64_t expireTimeNs = timer->getExpireTime();

                // 计算剩余时间（纳秒）
                if (expireTimeNs <= nowNs) {
                    // 已经过期，立即执行
                    timer->handleTimeout();
                    --m_size;
                    continue;
                }

                uint64_t remainingNs = expireTimeNs - nowNs;

                // 计算剩余的 tick 数
                uint64_t remainingTicks = remainingNs / m_tickDuration;

                if (remainingTicks == 0) {
                    // 不足一个 tick，立即执行
                    timer->handleTimeout();
                    --m_size;
                    continue;
                }

                // 计算绝对到期 tick
                uint64_t absoluteTick = m_currentTick + remainingTicks;

                // 重新分配到合适的层（使用与 push 相同的逻辑）
                if (remainingTicks < WHEEL1_SPAN) {
                    size_t slot_idx = absoluteTick & (WHEEL1_SIZE - 1);
                    m_wheel1[slot_idx].push_back(timer);
                } else if (remainingTicks < WHEEL2_SPAN) {
                    size_t slot_idx = (absoluteTick >> 8) & (WHEEL2_SIZE - 1);
                    m_wheel2[slot_idx].push_back(timer);
                } else if (remainingTicks < WHEEL3_SPAN) {
                    size_t slot_idx = (absoluteTick >> 14) & (WHEEL3_SIZE - 1);
                    m_wheel3[slot_idx].push_back(timer);
                } else if (remainingTicks < WHEEL4_SPAN) {
                    size_t slot_idx = (absoluteTick >> 20) & (WHEEL4_SIZE - 1);
                    m_wheel4[slot_idx].push_back(timer);
                } else if (remainingTicks < WHEEL5_SPAN) {
                    size_t slot_idx = (absoluteTick >> 26) & (WHEEL5_SIZE - 1);
                    m_wheel5[slot_idx].push_back(timer);
                } else {
                    // 超出范围，丢弃
                    --m_size;
                }
            }
        }

        /**
         * @brief 将时间轮的起始时间和当前 tick 重置为当前时刻
         */
        void reset()
        {
            auto now = std::chrono::steady_clock::now();
            m_startTime = now;
            m_currentTick = 0;
        }

        uint64_t m_tickDuration;                               ///< 每 tick 的纳秒数
        uint64_t m_currentTick;                                ///< 当前 tick 位置

        std::vector<TimerList> m_wheel1;                       ///< 第1层时间轮
        std::vector<TimerList> m_wheel2;                       ///< 第2层时间轮
        std::vector<TimerList> m_wheel3;                       ///< 第3层时间轮
        std::vector<TimerList> m_wheel4;                       ///< 第4层时间轮
        std::vector<TimerList> m_wheel5;                       ///< 第5层时间轮

        size_t m_size;                                         ///< 活跃定时器总数
        std::chrono::steady_clock::time_point m_startTime;     ///< 时间轮纪元时间点
    };

}


#endif
