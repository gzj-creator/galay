/**
 * @file etcd_client.h
 * @brief etcd 同步客户端定义
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 基于同步阻塞 socket 的 etcd 客户端实现，支持：
 *          - 连接管理(Connect/Close)
 *          - KV 操作(Put/Get/Delete)
 *          - 租约操作(GrantLease/KeepAlive)
 *          - Pipeline 事务
 *          通过 Builder 模式构建客户端，所有操作均为同步阻塞调用。
 */

#ifndef GALAY_ETCD_SYNC_CLIENT_H
#define GALAY_ETCD_SYNC_CLIENT_H

#include "../base/etcd_config.h"
#include "../base/etcd_error.h"
#include "../base/network_cfg.h"
#include "../base/etcd_types.h"
#include "../base/etcd_value.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace galay::etcd
{

class EtcdClient;

/**
 * @brief 同步 etcd 客户端构建器
 * @details 使用 Builder 模式逐步配置并构建 EtcdClient 实例。
 *          支持链式调用设置端点地址、API 前缀、超时时间等参数。
 *
 * @code
 * auto client = EtcdClientBuilder()
 *     .endpoint("http://127.0.0.1:2379")
 *     .requestTimeout(std::chrono::seconds(5))
 *     .build();
 * @endcode
 */
class EtcdClientBuilder
{
public:
    /**
     * @brief 设置 etcd 服务端点地址
     * @param endpoint 端点地址，格式为 http(s)://host:port
     * @return 构建器引用，支持链式调用
     */
    EtcdClientBuilder& endpoint(std::string endpoint)
    {
        m_config.endpoint = std::move(endpoint);
        return *this;
    }

    /**
     * @brief 设置 API 路径前缀
     * @param prefix 路径前缀，如 "/v3"
     * @return 构建器引用，支持链式调用
     */
    EtcdClientBuilder& apiPrefix(std::string prefix)
    {
        m_config.api_prefix = std::move(prefix);
        return *this;
    }

    /**
     * @brief 设置生产模式配置
     * @param config 多端点、重试与健康检查配置
     * @return 构建器引用，支持链式调用
     * @note 当前仅保存配置并在 endpoints 非空时同步首个 endpoint，
     *       不改变现有单端点请求行为。
     */
    EtcdClientBuilder& productionConfig(EtcdProductionConfig config)
    {
        if (!config.endpoints.empty()) {
            m_config.endpoint = config.endpoints.front();
        }
        m_config.production = std::move(config);
        return *this;
    }

    /**
     * @brief 设置请求超时时间
     * @param timeout 超时时间
     * @return 构建器引用，支持链式调用
     */
    EtcdClientBuilder& requestTimeout(std::chrono::milliseconds timeout)
    {
        m_config.request_timeout = timeout;
        return *this;
    }

    /**
     * @brief 设置网络缓冲区大小
     * @param size 缓冲区大小（字节）
     * @return 构建器引用，支持链式调用
     */
    EtcdClientBuilder& bufferSize(size_t size)
    {
        m_config.buffer_size = size;
        return *this;
    }

    /**
     * @brief 设置是否启用 TCP keepalive
     * @param enabled 是否启用
     * @return 构建器引用，支持链式调用
     */
    EtcdClientBuilder& keepAlive(bool enabled)
    {
        m_config.keepalive = enabled;
        return *this;
    }

    /**
     * @brief 设置连接 socket 是否启用 TCP_NODELAY
     * @param enabled true 表示启用 TCP_NODELAY；false 表示保留系统默认
     * @return 构建器引用，支持链式调用
     */
    EtcdClientBuilder& tcpNoDelay(bool enabled)
    {
        m_config.tcp_no_delay = enabled;
        return *this;
    }

    /**
     * @brief 直接设置完整的客户端配置
     * @param config 同步客户端配置
     * @return 构建器引用，支持链式调用
     */
    EtcdClientBuilder& config(EtcdConfig config)
    {
        m_config = std::move(config);
        return *this;
    }

    /**
     * @brief 构建同步 etcd 客户端
     * @return 配置好的 EtcdClient 实例
     */
    EtcdClient build() const;

    /**
     * @brief 仅构建配置对象而不创建客户端
     * @return 配置好的 EtcdConfig 实例
     */
    EtcdConfig& buildConfig()
    {
        return m_config;
    }

    /**
     * @brief 仅查看构建配置对象而不创建客户端
     * @return 当前配置引用
     */
    const EtcdConfig& buildConfig() const
    {
        return m_config;
    }

private:
    EtcdConfig m_config{}; ///< 同步客户端配置
};

/**
 * @brief etcd 同步客户端
 * @details 基于阻塞 socket 的同步 etcd 客户端，通过 HTTP 协议与
 *          etcd v3 REST API 通信。所有操作均为同步阻塞调用，
 *          返回 std::expected<T, EtcdError> 携带结果或错误。
 * @note 该类不可拷贝、不可移动。
 */
class EtcdClient
{
public:
    using PipelineOpType = galay::etcd::PipelineOpType;         ///< Pipeline 操作类型
    using PipelineOp = galay::etcd::PipelineOp;                 ///< Pipeline 操作描述
    using PipelineItemResult = galay::etcd::PipelineItemResult; ///< Pipeline 操作结果

    /**
     * @brief 构造同步 etcd 客户端
     * @param config 同步客户端配置
     */
    explicit EtcdClient(EtcdConfig config = {});
    ~EtcdClient(); ///< 析构函数，关闭连接

    EtcdClient(const EtcdClient&) = delete;
    EtcdClient& operator=(const EtcdClient&) = delete;
    EtcdClient(EtcdClient&&) = delete;
    EtcdClient& operator=(EtcdClient&&) = delete;

    /**
     * @brief 同步连接到 etcd 服务端
     * @return 连接成功返回 true，失败返回 EtcdError
     */
    EtcdBoolResult connect();

    /**
     * @brief 同步关闭与 etcd 服务端的连接
     * @return 关闭成功返回 true，失败返回 EtcdError
     */
    EtcdBoolResult close();

    /**
     * @brief 同步写入键值对
     * @param key 键名
     * @param value 值
     * @param lease_id 可选的租约 ID，绑定后键在租约过期时自动删除
     * @return 写入成功返回 true，失败返回 EtcdError
     */
    EtcdBoolResult put(const std::string& key,
                       const std::string& value,
                       std::optional<int64_t> lease_id = std::nullopt);

    /**
     * @brief 同步读取键值对
     * @param key 键名
     * @param prefix 是否为前缀查询，为 true 时返回所有以 key 为前缀的键值对
     * @param limit 返回数量限制
     * @return 成功返回键值对列表，失败返回 EtcdError
     */
    EtcdGetResult get(const std::string& key,
                      bool prefix = false,
                      std::optional<int64_t> limit = std::nullopt);

    /**
     * @brief 同步删除键值对
     * @param key 键名
     * @param prefix 是否为前缀删除
     * @return 成功返回删除数量，失败返回 EtcdError
     */
    EtcdDeleteResult del(const std::string& key, bool prefix = false);

    /**
     * @brief 同步申请租约
     * @param ttl_seconds 租约存活时间（秒）
     * @return 成功返回租约 ID，失败返回 EtcdError
     */
    EtcdLeaseGrantResult grantLease(int64_t ttl_seconds);

    /**
     * @brief 同步发送一次租约续期请求
     * @param lease_id 需要续期的租约 ID
     * @return 成功返回租约 ID，失败返回 EtcdError
     */
    EtcdLeaseGrantResult keepAliveOnce(int64_t lease_id);

    /**
     * @brief 同步执行 Pipeline 事务（span 版本）
     * @param operations Pipeline 操作列表
     * @return 成功返回 Pipeline 操作结果列表，失败返回 EtcdError
     */
    EtcdPipelineResult pipeline(std::span<const PipelineOp> operations);

    /**
     * @brief 同步执行 Pipeline 事务（vector 版本）
     * @param operations Pipeline 操作列表
     * @return 成功返回 Pipeline 操作结果列表，失败返回 EtcdError
     */
    EtcdPipelineResult pipeline(std::vector<PipelineOp> operations);

    /**
     * @brief 检查客户端是否已连接
     * @return 已连接返回 true，否则返回 false
     */
    [[nodiscard]] bool connected() const;

    /**
     * @brief 获取统计快照
     * @details 当前同步 client 暂未在普通单端点路径中累计指标，
     *          返回值用于锁定公开 API 形状，后续生产 wrapper 会补齐计数。
     * @return 当前统计快照
     */
    [[nodiscard]] EtcdClientStats getStats() const
    {
        return m_stats;
    }

private:
    void resetLastOperation();
    void setError(EtcdErrorType type, const std::string& message);
    void setError(EtcdError error);
    EtcdVoidResult applySocketTimeout(std::optional<std::chrono::milliseconds> timeout);
    std::expected<std::string, EtcdError> postJsonInternal(
        const std::string& api_path,
        std::string body,
        std::optional<std::chrono::milliseconds> force_timeout = std::nullopt);

private:
    EtcdConfig m_config;                                                ///< 客户端配置
    EtcdNetworkConfig m_network_config;                                 ///< 网络配置
    std::string m_api_prefix;                                           ///< API 路径前缀
    std::string m_host_header;                                          ///< HTTP Host 头
    std::string m_endpoint_host;                                        ///< 端点主机名
    uint16_t m_endpoint_port = 0;                                       ///< 端点端口号
    bool m_endpoint_secure = false;                                     ///< 是否为 HTTPS 连接
    bool m_endpoint_ipv6 = false;                                       ///< 是否为 IPv6 地址
    std::string m_endpoint_error;                                       ///< 端点解析错误信息
    bool m_endpoint_valid = false;                                      ///< 端点地址是否有效

    int m_socket_fd = -1;                                               ///< TCP 套接字文件描述符
    bool m_connected = false;                                           ///< 是否已连接
    std::optional<std::chrono::milliseconds> m_applied_socket_timeout;  ///< 已应用的 socket 超时时间
    bool m_socket_timeout_cached = false;                               ///< socket 超时是否已缓存
    std::string m_request_buffer;                                       ///< 请求缓冲区
    std::string m_response_raw_buffer;                                  ///< 原始响应缓冲区
    std::vector<char> m_recv_buffer;                                    ///< 接收缓冲区

    EtcdError m_last_error;                                             ///< 最后一次错误
    EtcdClientStats m_stats{};                                          ///< 统计快照占位
};

} // namespace galay::etcd

inline galay::etcd::EtcdClient galay::etcd::EtcdClientBuilder::build() const
{
    return EtcdClient(m_config);
}

#endif // GALAY_ETCD_SYNC_CLIENT_H
