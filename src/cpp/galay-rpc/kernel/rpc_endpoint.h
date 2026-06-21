/**
 * @file rpc_endpoint.h
 * @brief RPC服务发现端点模型
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 定义服务发现和注册中心使用的完整endpoint信息。该类型不替换
 *          连接池已有的轻量RpcEndpoint，以保持Phase 5托管客户端兼容。
 */

#ifndef GALAY_RPC_ENDPOINT_H
#define GALAY_RPC_ENDPOINT_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace galay::rpc
{

/**
 * @brief endpoint服务状态
 */
enum class RpcEndpointStatus {
    Serving,      ///< 可接流量
    Draining,     ///< 正在摘流，不再选中
    Unavailable   ///< 不可用
};

/**
 * @brief RPC服务发现endpoint完整信息
 *
 * @details weight为0或status不是Serving时，负载选择器必须跳过该endpoint。
 */
struct RpcEndpointInfo {
    std::string host;  ///< 主机地址
    uint16_t port = 0; ///< 端口，0为无效值
    std::string service;  ///< 服务名
    std::string instance_id;  ///< 实例ID，同一服务内唯一
    uint32_t weight = 100;  ///< 权重，0表示不可选
    RpcEndpointStatus status = RpcEndpointStatus::Serving;  ///< 服务状态
    std::string version;  ///< 版本标签
    std::string zone;  ///< 可用区/机房标签
    std::unordered_map<std::string, std::string> metadata;  ///< 扩展元数据

    /// @brief 返回 host:port 地址字符串
    std::string address() const { return host + ":" + std::to_string(port); }

    /// @brief 返回稳定endpoint key
    std::string key() const { return service + "/" + instance_id; }

    /// @brief 判断endpoint是否可被负载选择
    bool selectable() const {
        return !host.empty() && port > 0 && !service.empty() && !instance_id.empty() &&
               weight > 0 && status == RpcEndpointStatus::Serving;
    }
};

} // namespace galay::rpc

#endif // GALAY_RPC_ENDPOINT_H
