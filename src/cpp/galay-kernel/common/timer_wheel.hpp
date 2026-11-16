/**
 * @file timer_wheel.hpp
 * @brief 分层时间轮共用的容量、槽位和到期 tick 计算
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 为单线程和多线程时间轮提供相同的五层几何定义。这里只计算纯数值，
 *          不持有定时器、时钟或队列状态；容量、计数和并发协议仍由各管理器负责。
 */

#ifndef GALAY_KERNEL_TIMER_WHEEL_HPP
#define GALAY_KERNEL_TIMER_WHEEL_HPP

#include <cstddef>
#include <cstdint>

namespace galay::kernel::detail
{

/** 分层时间轮共用的五层槽位容量和跨度。 */
struct TimerWheelGeometry
{
    static constexpr size_t kWheel1Size = 256;  ///< 第 1 层槽位数量
    static constexpr size_t kWheel2Size = 64;   ///< 第 2 层槽位数量
    static constexpr size_t kWheel3Size = 64;   ///< 第 3 层槽位数量
    static constexpr size_t kWheel4Size = 64;   ///< 第 4 层槽位数量
    static constexpr size_t kWheel5Size = 64;   ///< 第 5 层槽位数量

    static constexpr uint64_t kWheel1Span = kWheel1Size;                        ///< 第 1 层跨度
    static constexpr uint64_t kWheel2Span = kWheel1Span * kWheel2Size;          ///< 第 2 层跨度
    static constexpr uint64_t kWheel3Span = kWheel2Span * kWheel3Size;          ///< 第 3 层跨度
    static constexpr uint64_t kWheel4Span = kWheel3Span * kWheel4Size;          ///< 第 4 层跨度
    static constexpr uint64_t kWheel5Span = kWheel4Span * kWheel5Size;          ///< 第 5 层跨度

    static constexpr size_t kLastWheelIndex = 4;  ///< 最后一层的零基索引

    /** @brief 根据相对 tick 延迟返回时间轮层号；超出范围返回 `kLastWheelIndex`。 */
    static constexpr size_t layer_for_delay(uint64_t delay_ticks) noexcept
    {
        if (delay_ticks < kWheel1Span) {
            return 0;
        }
        if (delay_ticks < kWheel2Span) {
            return 1;
        }
        if (delay_ticks < kWheel3Span) {
            return 2;
        }
        if (delay_ticks < kWheel4Span) {
            return 3;
        }
        return kLastWheelIndex;
    }

    /** @brief 根据相对 tick 延迟返回绝对到期 tick。 */
    static constexpr uint64_t expiry_tick(uint64_t current_tick,
                                          uint64_t delay_ticks) noexcept
    {
        return current_tick + delay_ticks;
    }

    /** @brief 返回指定层在当前 tick 对应的槽位。 */
    static constexpr size_t slot_index(size_t layer,
                                       uint64_t tick) noexcept
    {
        if (layer == 0) {
            return tick & (kWheel1Size - 1);
        }
        if (layer == 1) {
            return (tick >> 8) & (kWheel2Size - 1);
        }
        if (layer == 2) {
            return (tick >> 14) & (kWheel3Size - 1);
        }
        if (layer == 3) {
            return (tick >> 20) & (kWheel4Size - 1);
        }
        return (tick >> 26) & (kWheel5Size - 1);
    }

    /** @brief 判断绝对 tick 是否到达该层向下一层级联的边界。 */
    static constexpr bool is_cascade_tick(size_t layer,
                                          uint64_t tick) noexcept
    {
        if (layer == 0) {
            return (tick & (kWheel1Size - 1)) == 0 && tick > 0;
        }
        if (layer == 1) {
            return (tick & (kWheel2Span - 1)) == 0 && tick > 0;
        }
        if (layer == 2) {
            return (tick & (kWheel3Span - 1)) == 0 && tick > 0;
        }
        return (tick & (kWheel4Span - 1)) == 0 && tick > 0;
    }

    /** @brief 将纳秒剩余时长向上取整为 tick，确保放在截止时刻或之后。 */
    static constexpr uint64_t ceil_ticks(uint64_t remaining_ns,
                                         uint64_t tick_duration_ns) noexcept
    {
        return remaining_ns / tick_duration_ns +
               (remaining_ns % tick_duration_ns != 0);
    }

    /** @brief 将纳秒剩余时长向下取整为 tick。 */
    static constexpr uint64_t floor_ticks(uint64_t remaining_ns,
                                          uint64_t tick_duration_ns) noexcept
    {
        return remaining_ns / tick_duration_ns;
    }
};

} // namespace galay::kernel::detail

#endif // GALAY_KERNEL_TIMER_WHEEL_HPP
