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

#include <string>

namespace galay::etcd
{

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
