/**
 * @file conn_pool_waiter_state.h
 * @brief Redis pool waiter completion state
 * @author galay-redis
 * @version 1.0.0
 */

#ifndef GALAY_REDIS_CONNECTION_POOL_WAITER_STATE_H
#define GALAY_REDIS_CONNECTION_POOL_WAITER_STATE_H

#include <atomic>
#include <cstdint>

namespace galay::redis::detail
{
    /**
     * @brief 连接池等待者完成状态
     * @details release、timeout 和 cancel 通过该状态做单一所有权转移；
     *          只有第一个从 Waiting CAS 成功的回调可以写入最终结果并唤醒协程。
     */
    enum class PoolWaiterState : uint8_t
    {
        Waiting,
        Completed,
        TimedOut,
        Cancelled,
    };

    /**
     * @brief 尝试完成等待者
     * @param state 等待者共享状态
     * @param target 完成后的目标状态
     * @return 从 Waiting 成功切换到 target 返回 true，否则返回 false
     * @note 非阻塞；用于协程等待者的 release/timeout/cancel 仲裁。
     */
    inline bool try_complete_waiter(std::atomic<PoolWaiterState>& state,
                                    PoolWaiterState target) noexcept
    {
        PoolWaiterState expected = PoolWaiterState::Waiting;
        return state.compare_exchange_strong(expected,
                                             target,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire);
    }
} // namespace galay::redis::detail

#endif
