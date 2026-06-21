/**
 * @file rpc_policy.h
 * @brief RPC路由治理策略基础类型
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 定义按 service/method/mode 精确匹配的路由策略 key 与策略值。
 */

#ifndef GALAY_RPC_POLICY_H
#define GALAY_RPC_POLICY_H

#include "../protoc/rpc_base.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace galay::rpc
{

/**
 * @brief RPC路由策略匹配键。
 *
 * @details service、method 与调用模式共同确定一个治理策略槽位。该类型为值语义，
 *          可作为 `std::unordered_map` key；读取时不阻塞，不涉及协程调度。
 */
struct RpcRouteKey {
    std::string service;       ///< 服务名，空字符串表示无效配置。
    std::string method;        ///< 方法名，空字符串表示无效配置。
    RpcCallMode mode = RpcCallMode::UNARY; ///< 调用模式粒度。

    bool operator==(const RpcRouteKey& other) const noexcept = default;
};

/**
 * @brief RpcRouteKey 哈希器。
 */
struct RpcRouteKeyHash {
    std::size_t operator()(const RpcRouteKey& key) const noexcept
    {
        const std::size_t h1 = std::hash<std::string>{}(key.service);
        const std::size_t h2 = std::hash<std::string>{}(key.method);
        const std::size_t h3 = std::hash<unsigned>{}(static_cast<unsigned>(key.mode));
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6U) + (h1 >> 2U)) ^
               (h3 + 0x9e3779b97f4a7c15ULL + (h2 << 6U) + (h2 >> 2U));
    }
};

/**
 * @brief RPC路由级治理策略。
 *
 * @details 当前只提供 Phase 2 所需的基础字段。后续 retry、熔断、限流等策略可在
 *          Phase 4 继续扩展，但调用方已可按 service/method/mode 读取稳定快照。
 */
struct RpcRoutePolicy {
    int timeout_ms = 30000;                 ///< 单次调用超时，毫秒，0 表示不设置超时。
    int deadline_ms = 30000;                ///< 总 deadline，毫秒，0 表示不设置 deadline。
    std::uint32_t max_outstanding_requests = 1024; ///< 路由级最大并发请求数，必须大于 0。
    std::uint32_t max_message_size = RPC_MAX_BODY_SIZE; ///< 路由级最大消息体大小，必须大于 0。
};

using RpcRoutePolicyMap = std::unordered_map<RpcRouteKey, RpcRoutePolicy, RpcRouteKeyHash>;

/**
 * @brief 在策略表中查找精确路由策略，缺失时返回默认策略。
 * @param policies 路由策略表。
 * @param default_policy 缺省策略引用，调用方需保证生命周期覆盖返回引用。
 * @param service 服务名。
 * @param method 方法名。
 * @param mode 调用模式。
 * @return 命中时返回 map 中策略，否则返回 default_policy。
 * @note 该函数只读内存，不阻塞，不分配协程任务。
 */
inline const RpcRoutePolicy& findRpcRoutePolicy(const RpcRoutePolicyMap& policies,
                                                const RpcRoutePolicy& default_policy,
                                                std::string_view service,
                                                std::string_view method,
                                                RpcCallMode mode)
{
    const auto it = policies.find(RpcRouteKey{std::string(service), std::string(method), mode});
    if (it == policies.end()) {
        return default_policy;
    }
    return it->second;
}

} // namespace galay::rpc

#endif // GALAY_RPC_POLICY_H
