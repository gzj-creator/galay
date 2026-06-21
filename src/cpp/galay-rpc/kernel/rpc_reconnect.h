/**
 * @file rpc_reconnect.h
 * @brief RPC客户端重连策略
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供opt-in客户端重连配置。策略只作用于连接建立和下一次新调用前
 *          的重连，不会自动重放已经发出的pending调用。
 */

#ifndef GALAY_RPC_RECONNECT_H
#define GALAY_RPC_RECONNECT_H

#include <chrono>
#include <cstddef>

namespace galay::rpc
{

/**
 * @brief RPC客户端重连策略
 *
 * @details 默认max_attempts为1，表示不重试。设置为大于1后，客户端connect()
 *          会在失败后异步等待backoff再重试；连接断开后的下一次call也会先尝试
 *          重新建立连接。backoff通过协程sleep挂起，不阻塞Task路径。
 */
struct RpcReconnectPolicy {
    size_t max_attempts = 1;                         ///< 总连接尝试次数，1表示不重试
    std::chrono::milliseconds backoff{0};            ///< 每次失败后的异步退避时间
};

} // namespace galay::rpc

#endif // GALAY_RPC_RECONNECT_H
