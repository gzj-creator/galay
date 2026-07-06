/**
 * @file etcd_config.h
 * @brief etcd 客户端配置结构定义
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 定义同步/异步 etcd 客户端使用的配置结构，
 *          继承自网络配置(EtcdNetworkConfig)，额外包含 etcd 服务端点
 *          和 API 路径前缀等配置项。
 */

#ifndef GALAY_ETCD_CONFIG_H
#define GALAY_ETCD_CONFIG_H

#include "network_cfg.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace galay::etcd
{

/**
 * @brief etcd 多端点选择策略
 * @details 该枚举只定义生产模式下期望的端点选择语义，
 *          当前单端点 client 仍以 `endpoint` 字段作为实际请求地址。
 */
enum class EtcdEndpointPolicy
{
    FirstHealthy, ///< 优先选择首个健康端点
    RoundRobin,   ///< 在健康端点之间轮询
    StickyLeader, ///< 优先粘附 leader 端点
};

/**
 * @brief etcd 重试决策
 * @details 为后续多端点 client 暴露统一的重试动作枚举。
 */
enum class EtcdRetryDecision
{
    RetrySameEndpoint, ///< 在当前端点重试
    RetryNextEndpoint, ///< 切换到下一个端点重试
    FailFast,          ///< 立即失败
};

/**
 * @brief etcd 生产重试配置
 * @details 当前仅作为公开配置面的一部分保存默认值，
 *          具体重试逻辑由后续 cluster wrapper 接入。
 */
struct EtcdRetryConfig
{
    size_t attempts = 3;                                        ///< 最大尝试次数
    std::chrono::milliseconds initial_backoff{25};              ///< 初始退避
    std::chrono::milliseconds max_backoff{500};                 ///< 最大退避
    bool jitter = true;                                         ///< 是否启用抖动
};

/**
 * @brief etcd 鉴权配置
 * @details 保存用户名/密码或 bearer token，并提供脱敏输出，
 *          供调试、日志和未来 auth wrapper 复用。
 */
struct EtcdCredentialConfig
{
    std::string username;      ///< 用户名
    std::string password;      ///< 密码
    std::string bearer_token;  ///< Bearer token

    /**
     * @brief 返回脱敏后的配置字符串
     * @return 不包含明文 password / bearer token 的调试字符串
     */
    [[nodiscard]] std::string redactedString() const
    {
        std::string out = "EtcdCredentialConfig{username=";
        out += username.empty() ? "<empty>" : username;
        out += ", password=";
        out += password.empty() ? "<empty>" : "<redacted>";
        out += ", bearer_token=";
        out += bearer_token.empty() ? "<empty>" : "<redacted>";
        out += "}";
        return out;
    }
};

/**
 * @brief etcd 生产模式配置
 * @details 保存多端点、健康检查和重试策略。
 *          当前单端点 client 会仅将首个 endpoint 回填到 `EtcdConfig::endpoint`，
 *          以保持既有请求路径不变。
 */
struct EtcdProductionConfig
{
    std::vector<std::string> endpoints;                         ///< etcd 集群端点列表
    EtcdRetryConfig retry;                                      ///< 重试配置
    std::chrono::milliseconds health_interval{5000};           ///< 健康检查间隔
    EtcdEndpointPolicy endpoint_policy = EtcdEndpointPolicy::FirstHealthy; ///< 端点选择策略
    bool prefer_leader = false;                                 ///< 是否优先 leader
};

/**
 * @brief etcd 同步/异步客户端共用配置
 * @details 继承 EtcdNetworkConfig 的网络配置参数，
 *          增加服务端点地址和 API 路径前缀配置。
 *          用于 EtcdClient、AsyncEtcdClient 及其 Builder。
 */
struct EtcdConfig : EtcdNetworkConfig
{
    std::string endpoint = "http://127.0.0.1:2379"; ///< etcd 服务端点地址
    std::string api_prefix = "/v3";                 ///< API 路径前缀
    EtcdProductionConfig production;                ///< 生产多端点与重试配置
    EtcdCredentialConfig credentials;               ///< 鉴权与脱敏配置

    /**
     * @brief 创建一个指定超时时间的配置
     * @param timeout 请求超时时间
     * @return 配置好超时时间的 EtcdConfig 实例
     */
    static EtcdConfig withTimeout(std::chrono::milliseconds timeout)
    {
        EtcdConfig cfg;
        cfg.request_timeout = timeout;
        return cfg;
    }
};

} // namespace galay::etcd

#endif // GALAY_ETCD_CONFIG_H
