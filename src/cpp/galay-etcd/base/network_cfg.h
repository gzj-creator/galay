/**
 * @file network_cfg.h
 * @brief etcd 网络层配置结构定义
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 定义 etcd 客户端的网络层配置参数，包括请求超时时间、
 *          缓冲区大小和 TCP keepalive 设置。
 *          该结构是 EtcdConfig 的基类。
 */

#ifndef GALAY_ETCD_NETWORK_CONFIG_H
#define GALAY_ETCD_NETWORK_CONFIG_H

#include <chrono>
#include <cstddef>

namespace galay::etcd
{

/**
 * @brief etcd 网络层配置
 * @details 包含 etcd 客户端网络通信相关的配置参数，
 *          作为其他更具体配置结构的基类使用。
 */
struct EtcdNetworkConfig
{
    std::chrono::milliseconds request_timeout = std::chrono::milliseconds(-1); ///< 请求超时时间，-1 表示不超时
    size_t buffer_size = 16384;   ///< 网络收发缓冲区大小（字节）
    bool keepalive = true;        ///< 是否启用 TCP keepalive
    bool tcp_no_delay = true;     ///< 是否为连接 socket 启用 TCP_NODELAY

    /**
     * @brief 判断是否启用了请求超时
     * @return 若超时时间 >= 0ms 则返回 true，否则返回 false
     */
    [[nodiscard]] bool isRequestTimeoutEnabled() const
    {
        return request_timeout >= std::chrono::milliseconds(0);
    }

    /**
     * @brief 创建一个指定超时时间的网络配置
     * @param timeout 请求超时时间
     * @return 配置好超时时间的 EtcdNetworkConfig 实例
     */
    static EtcdNetworkConfig withTimeout(std::chrono::milliseconds timeout)
    {
        EtcdNetworkConfig cfg;
        cfg.request_timeout = timeout;
        return cfg;
    }
};

} // namespace galay::etcd

#endif // GALAY_ETCD_NETWORK_CONFIG_H
