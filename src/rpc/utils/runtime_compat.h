/**
 * @file runtime_compat.h
 * @brief RPC运行时兼容性辅助函数
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供RPC服务器/客户端运行时配置相关的辅助工具函数，
 *          例如根据硬件并发度自动推导IO调度器数量。
 */

#ifndef GALAY_RPC_RUNTIME_COMPAT_H
#define GALAY_RPC_RUNTIME_COMPAT_H

#include <algorithm>
#include <cstddef>
#include <thread>

namespace galay::rpc
{

/**
 * @brief 解析IO调度器数量
 * @param requested 用户请求的调度器数量，0表示自动检测
 * @return 实际使用的调度器数量，至少为1
 */
inline size_t resolveIoSchedulerCount(size_t requested)
{
    if (requested != 0) {
        return requested;
    }

    const auto detected = static_cast<size_t>(std::thread::hardware_concurrency());
    return std::max<size_t>(1, detected);
}

} // namespace galay::rpc

#endif // GALAY_RPC_RUNTIME_COMPAT_H
