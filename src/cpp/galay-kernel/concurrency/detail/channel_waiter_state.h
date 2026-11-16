/**
 * @file channel_waiter_state.h
 * @brief 异步通道等待体共用的原子状态。
 */

#ifndef GALAY_KERNEL_CONCURRENCY_DETAIL_CHANNEL_WAITER_STATE_H
#define GALAY_KERNEL_CONCURRENCY_DETAIL_CHANNEL_WAITER_STATE_H

#include <cstdint>

namespace galay::kernel::detail
{

/**
 * @brief 异步通道等待体在注册、认领、完成和回收阶段的状态。
 * @note Bounded SPSC 使用完整状态集合；其余通道只使用 waiting/failed 终态子集。
 */
enum class ChannelWaiterState : uint8_t
{
    kIdle,        ///< 固定 waiter 槽当前未被异步操作占用。
    kRegistering, ///< await_suspend 正在初始化尚未发布的 waiter 槽。
    kWaiting,     ///< 已注册但尚未被对端认领。
    kCancelled,   ///< 已由超时路径取消。
    kFulfilling,  ///< 对端已认领，正在搬运值。
    kFulfilled,   ///< 已成功交接。
    kClosed,      ///< 因通道关闭结束。
    kFailed,      ///< 等待队列无法接收 waiter，返回 kNotReady。
    kReclaiming,  ///< await_resume 正在等待旧推进方释放槽引用。
};

} // namespace galay::kernel::detail

#endif // GALAY_KERNEL_CONCURRENCY_DETAIL_CHANNEL_WAITER_STATE_H
