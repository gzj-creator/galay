/**
 * @file channel_backoff.h
 * @brief 有界通道 ring 竞争共用的分级退避。
 */

#ifndef GALAY_KERNEL_CONCURRENCY_DETAIL_CHANNEL_BACKOFF_H
#define GALAY_KERNEL_CONCURRENCY_DETAIL_CHANNEL_BACKOFF_H

#include "channel_cpu_pause.h"

#include <cstdint>
#include <thread>

namespace galay::kernel::detail
{

/**
 * @brief 有界 ring 在 CAS 竞争和发布窗口中的自适应退避。
 * @details spin() 只执行有上限的 CPU hint；snooze() 在持续等待超过自旋上限后
 *          才让出当前线程时间片。
 */
class ChannelBackoff
{
public:
    /**
     * @brief 对 cursor CAS 竞争执行有上限的指数自旋。
     * @return 无返回值
     * @note 仅发出 CPU hint，不调用 std::this_thread::yield()。
     */
    void spin() noexcept
    {
        const uint32_t step = m_step < kSpinLimit ? m_step : kSpinLimit;
        const uint32_t spins = uint32_t{1} << step;
        for (uint32_t i = 0; i < spins; ++i) {
            channel_cpu_pause();
        }
        if (m_step <= kSpinLimit) {
            ++m_step;
        }
    }

    /**
     * @brief 等待 slot sequence 发布时执行分级退避。
     * @return 无返回值
     * @note 初始阶段使用 CPU hint；持续竞争超过自旋上限后才让出当前线程时间片。
     */
    void snooze() noexcept
    {
        if (m_step <= kSpinLimit) {
            const uint32_t spins = uint32_t{1} << m_step;
            for (uint32_t i = 0; i < spins; ++i) {
                channel_cpu_pause();
            }
        } else {
            std::this_thread::yield();
        }
        if (m_step < kRetryLimit) {
            ++m_step;
        }
    }

private:
    static constexpr uint32_t kSpinLimit = 6;
    static constexpr uint32_t kRetryLimit = 10;
    uint32_t m_step = 0;
};

} // namespace galay::kernel::detail

#endif // GALAY_KERNEL_CONCURRENCY_DETAIL_CHANNEL_BACKOFF_H
