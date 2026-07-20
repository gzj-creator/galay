/**
 * @file etcd_client.h
 * @brief etcd 异步客户端定义
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 基于 C++20 协程的异步 etcd 客户端实现，支持：
 *          - 连接管理(Connect/Close)
 *          - KV 操作(Put/Get/Delete)
 *          - 租约操作(GrantLease/KeepAlive)
 *          - Pipeline 事务
 *          - Watch 事件监听
 *          通过 Builder 模式构建客户端，所有操作返回 Awaitable 对象，
 *          可在 co_await 表达式中使用。
 */

#ifndef GALAY_ETCD_ASYNC_ETCD_CLIENT_H
#define GALAY_ETCD_ASYNC_ETCD_CLIENT_H

#include "../base/etcd_config.h"
#include "../base/etcd_error.h"
#include "../base/network_cfg.h"
#include "../base/etcd_types.h"
#include "../base/etcd_value.h"

#include "../../galay-http/kernel/http_session.h"
#include "../../galay-kernel/async/tcp_socket.h"
#include "../../galay-kernel/common/host.hpp"
#include "../../galay-kernel/core/io_scheduler.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <coroutine>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::etcd
{

class AsyncEtcdClient;

namespace details
{
class ConnectAwaitable;
class CloseAwaitable;
class PostJsonAwaitable;
class JsonOpAwaitableBase;
class PutAwaitable;
class GetAwaitable;
class DeleteAwaitable;
class GrantLeaseAwaitable;
class KeepAliveAwaitable;
class PipelineAwaitable;
struct AsyncEtcdClientPoolState;

template <typename TaskType>
struct AsyncEtcdTaskValue;

template <typename T>
struct AsyncEtcdTaskValue<galay::kernel::Task<T>>
{
    using type = T;
};

template <class Fn>
using AsyncEtcdOperationResult = typename AsyncEtcdTaskValue<
    std::remove_cvref_t<std::invoke_result_t<Fn&, AsyncEtcdClient&>>>::type;

template <class Fn>
using AsyncEtcdOperationValue = typename AsyncEtcdOperationResult<Fn>::value_type;
} // namespace details

/**
 * @brief etcd 异步客户端
 * @details 基于 C++20 协程和 galay-kernel IO 调度器的异步 etcd 客户端，
 *          通过 HTTP 协议与 etcd v3 REST API 通信。
 *          所有操作均返回 Awaitable 对象，支持 co_await 调用。
 * @note 该类不可拷贝、不可移动，生命周期内绑定固定的 IO 调度器。
 */
class AsyncEtcdClient
{
public:
    using PipelineOpType = galay::etcd::PipelineOpType;         ///< Pipeline 操作类型
    using PipelineOp = galay::etcd::PipelineOp;                 ///< Pipeline 操作描述
    using PipelineItemResult = galay::etcd::PipelineItemResult; ///< Pipeline 操作结果
    using WatchTaskHandler = std::function<galay::kernel::Task<void>(EtcdWatchResponse)>;   ///< Watch 协程回调处理器
    using WatchFunctionHandler = std::function<void(EtcdWatchResponse)>;                    ///< Watch 普通函数回调处理器
    using ConnectAwaitable = details::ConnectAwaitable;             ///< 连接操作 Awaitable
    using CloseAwaitable = details::CloseAwaitable;                 ///< 关闭操作 Awaitable
    using PostJsonAwaitable = details::PostJsonAwaitable;           ///< JSON POST 内部 Awaitable
    using PutAwaitable = details::PutAwaitable;                     ///< Put 操作 Awaitable
    using GetAwaitable = details::GetAwaitable;                     ///< Get 操作 Awaitable
    using DeleteAwaitable = details::DeleteAwaitable;               ///< Delete 操作 Awaitable
    using GrantLeaseAwaitable = details::GrantLeaseAwaitable;       ///< GrantLease 操作 Awaitable
    using KeepAliveAwaitable = details::KeepAliveAwaitable;         ///< KeepAlive 操作 Awaitable
    using PipelineAwaitable = details::PipelineAwaitable;           ///< Pipeline 操作 Awaitable

    /**
     * @brief 构造异步 etcd 客户端
     * @param scheduler IO 调度器指针
     * @param config 异步客户端配置
     */
    AsyncEtcdClient(galay::kernel::IOScheduler* scheduler,
                    EtcdConfig config = {});
    ~AsyncEtcdClient(); ///< 析构函数，关闭连接并停止 Watch

    AsyncEtcdClient(const AsyncEtcdClient&) = delete;
    AsyncEtcdClient& operator=(const AsyncEtcdClient&) = delete;
    AsyncEtcdClient(AsyncEtcdClient&&) = delete;
    AsyncEtcdClient& operator=(AsyncEtcdClient&&) = delete;

    /**
     * @brief 异步连接到 etcd 服务端
     * @return 连接操作 Awaitable，co_await 返回 EtcdBoolResult
     */
    ConnectAwaitable connect();

    /**
     * @brief 异步关闭与 etcd 服务端的连接
     * @return 关闭操作 Awaitable，co_await 返回 EtcdBoolResult
     */
    CloseAwaitable close();

    /**
     * @brief 异步写入键值对
     * @param key 键名
     * @param value 值
     * @param lease_id 可选的租约 ID，绑定后键在租约过期时自动删除
     * @return Put 操作 Awaitable，co_await 返回 EtcdBoolResult
     */
    PutAwaitable put(const std::string& key,
                     const std::string& value,
                     std::optional<int64_t> lease_id = std::nullopt);

    /**
     * @brief 异步读取键值对
     * @param key 键名
     * @param prefix 是否为前缀查询，为 true 时返回所有以 key 为前缀的键值对
     * @param limit 返回数量限制
     * @return Get 操作 Awaitable，co_await 返回 EtcdGetResult
     */
    GetAwaitable get(const std::string& key,
                     bool prefix = false,
                     std::optional<int64_t> limit = std::nullopt);

    /**
     * @brief 异步删除键值对
     * @param key 键名
     * @param prefix 是否为前缀删除
     * @return Delete 操作 Awaitable，co_await 返回 EtcdDeleteResult（删除数量）
     */
    DeleteAwaitable del(const std::string& key, bool prefix = false);

    /**
     * @brief 异步申请租约
     * @param ttl_seconds 租约存活时间（秒）
     * @return GrantLease 操作 Awaitable，co_await 返回 EtcdLeaseGrantResult（租约 ID）
     */
    GrantLeaseAwaitable grantLease(int64_t ttl_seconds);

    /**
     * @brief 异步发送一次租约续期请求
     * @param lease_id 需要续期的租约 ID
     * @return KeepAlive 操作 Awaitable，co_await 返回 EtcdLeaseGrantResult（租约 ID）
     */
    KeepAliveAwaitable keepAliveOnce(int64_t lease_id);

    /**
     * @brief 异步执行 Pipeline 事务（span 版本）
     * @param operations Pipeline 操作列表
     * @return Pipeline 操作 Awaitable，co_await 返回 EtcdPipelineResult
     */
    PipelineAwaitable pipeline(std::span<const PipelineOp> operations);

    /**
     * @brief 异步执行 Pipeline 事务（vector 版本）
     * @param operations Pipeline 操作列表
     * @return Pipeline 操作 Awaitable，co_await 返回 EtcdPipelineResult
     */
    PipelineAwaitable pipeline(std::vector<PipelineOp> operations);

    /**
     * @brief 获取统计快照
     * @details 当前异步 client 暂未在普通单端点路径中累计指标，
     *          返回值用于锁定公开 API 形状，后续生产 wrapper 会补齐计数。
     * @return 当前统计快照
     */
    [[nodiscard]] EtcdClientStats getStats() const
    {
        return m_stats;
    }

    /**
     * @brief 为单个 key 启动异步 watch，并把每个事件批次投递给 `Task<void>` 处理器。
     * @note handler 参数按值传递，避免协程 frame 持有悬空引用。
     * @note watch 由后台线程维持长连接；处理器本身会被调度到 client 绑定的 `IOScheduler`。
     */
    EtcdBoolResult watch(const std::string& key, WatchTaskHandler handler);

    /**
     * @brief 为单个 key 启动异步 watch，并在后台 watch 线程上直接调用普通函数处理器。
     * @note 如果需要在 `IOScheduler` 上继续协程化处理，请使用 `WatchTaskHandler` 重载。
     */
    EtcdBoolResult watch(const std::string& key, WatchFunctionHandler handler);

    /**
     * @brief 检查客户端是否已连接
     * @return 已连接返回 true，否则返回 false
     */
    [[nodiscard]] bool connected() const;

private:
    friend class details::ConnectAwaitable;
    friend class details::CloseAwaitable;
    friend class details::PostJsonAwaitable;
    friend class details::JsonOpAwaitableBase;
    friend class details::PutAwaitable;
    friend class details::GetAwaitable;
    friend class details::DeleteAwaitable;
    friend class details::GrantLeaseAwaitable;
    friend class details::KeepAliveAwaitable;
    friend class details::PipelineAwaitable;

    struct WatchWorkerState;

    void resetLastOperation();
    void setError(EtcdErrorType type, const std::string& message);
    void setError(EtcdError error);

    [[nodiscard]] EtcdBoolResult currentBoolResult() const;
    std::expected<std::string, EtcdError> resumePostOrCurrent(
        PostJsonAwaitable* post_awaitable);
    [[nodiscard]] std::string buildSerializedPostRequest(std::string_view api_path,
                                                         std::string_view body) const;

    PostJsonAwaitable postJsonInternal(const std::string& api_path,
                                       const std::string& body,
                                       std::optional<std::chrono::milliseconds> force_timeout = std::nullopt);
    EtcdBoolResult startWatchWorker(const std::string& key,
                                    std::function<void(EtcdWatchResponse)> dispatch);
    void stopWatchWorkers();
    void joinWatchWorkers();

private:
    galay::kernel::IOScheduler* m_scheduler;                              ///< IO 调度器指针
    EtcdConfig m_config;                                                   ///< 客户端配置
    EtcdNetworkConfig m_network_config;                                    ///< 网络配置
    std::string m_api_prefix;                                              ///< API 路径前缀
    std::string m_host_header;                                             ///< HTTP Host 头
    std::string m_serialized_request_prefix;                               ///< 序列化请求前缀缓存
    std::string m_serialized_request_headers;                              ///< 序列化请求头缓存
    std::optional<galay::kernel::Host> m_server_host;                      ///< 解析后的服务端主机地址
    std::string m_endpoint_error;                                          ///< 端点解析错误信息

    std::unique_ptr<galay::async::TcpSocket> m_socket;                     ///< TCP 套接字
    std::unique_ptr<galay::http::HttpSession> m_http_session;              ///< HTTP 会话

    EtcdError m_last_error;                                                ///< 最后一次错误
    EtcdClientStats m_stats{};                                             ///< 统计快照占位
    std::mutex m_watch_mutex;                                              ///< Watch 工作列表互斥锁
    std::vector<std::shared_ptr<WatchWorkerState>> m_watch_workers;        ///< Watch 工作线程状态列表
    galay::kernel::IPType m_ip_type = galay::kernel::IPType::IPV4;        ///< IP 协议类型
    bool m_endpoint_valid = false;                                         ///< 端点地址是否有效
    bool m_connected = false;                                              ///< 是否已连接
};

/**
 * @brief 异步 etcd 客户端构建器
 * @details 使用 Builder 模式逐步配置并构建 AsyncEtcdClient 实例。
 *          支持链式调用设置调度器、端点地址、API 前缀、超时时间等参数。
 *
 * @code
 * auto client = AsyncEtcdClientBuilder()
 *     .scheduler(&scheduler)
 *     .endpoint("http://127.0.0.1:2379")
 *     .requestTimeout(std::chrono::seconds(5))
 *     .build();
 * @endcode
 */
class AsyncEtcdClientBuilder
{
public:
    AsyncEtcdClientBuilder() = default;
    AsyncEtcdClientBuilder(AsyncEtcdClientBuilder&&) noexcept = default;
    AsyncEtcdClientBuilder& operator=(AsyncEtcdClientBuilder&&) noexcept = default;

    /**
     * @brief 设置 IO 调度器
     * @param scheduler IO 调度器指针
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& scheduler(galay::kernel::IOScheduler* scheduler)
    {
        m_scheduler = scheduler;
        return *this;
    }

    /**
     * @brief 设置 etcd 服务端点地址
     * @param endpoint 端点地址，格式为 http(s)://host:port
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& endpoint(std::string endpoint)
    {
        m_config.endpoint = std::move(endpoint);
        return *this;
    }

    /**
     * @brief 设置 API 路径前缀
     * @param prefix 路径前缀，如 "/v3"
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& apiPrefix(std::string prefix)
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
    AsyncEtcdClientBuilder& productionConfig(EtcdProductionConfig config)
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
    AsyncEtcdClientBuilder& requestTimeout(std::chrono::milliseconds timeout)
    {
        m_config.request_timeout = timeout;
        return *this;
    }

    /**
     * @brief 设置网络缓冲区大小
     * @param size 缓冲区大小（字节）
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& bufferSize(size_t size)
    {
        m_config.buffer_size = size;
        return *this;
    }

    /**
     * @brief 设置是否启用 TCP keepalive
     * @param enabled 是否启用
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& keepAlive(bool enabled)
    {
        m_config.keepalive = enabled;
        return *this;
    }

    /**
     * @brief 设置连接 socket 是否启用 TCP_NODELAY
     * @param enabled true 表示启用 TCP_NODELAY；false 表示保留系统默认
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& tcpNoDelay(bool enabled)
    {
        m_config.tcp_no_delay = enabled;
        return *this;
    }

    /**
     * @brief 直接设置完整的客户端配置
     * @param config 客户端配置
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& config(EtcdConfig config)
    {
        m_config = std::move(config);
        return *this;
    }

    /**
     * @brief 构建异步 etcd 客户端
     * @return 配置好的 AsyncEtcdClient 实例
     */
    AsyncEtcdClient build() const;

    /**
     * @brief 显式复制 builder 的离线配置状态
     * @return 当前 builder 的独立副本
     */
    [[nodiscard]] AsyncEtcdClientBuilder clone() const
    {
        return AsyncEtcdClientBuilder(*this);
    }

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
    AsyncEtcdClientBuilder(const AsyncEtcdClientBuilder&) = default;
    AsyncEtcdClientBuilder& operator=(const AsyncEtcdClientBuilder&) = default;

    galay::kernel::IOScheduler* m_scheduler = nullptr; ///< IO 调度器指针
    EtcdConfig m_config{};                             ///< 客户端配置
};

class AsyncEtcdClusterClient;

/**
 * @brief AsyncEtcdClient 池租约
 * @details move-only RAII 句柄，保证同一个 AsyncEtcdClient 同时只由一个调用协程使用。
 *          租约析构或调用 release() 时会把 client 归还无锁空闲队列。
 * @note client 仍绑定创建它的 IOScheduler；租约不会迁移或重新绑定 scheduler。
 * @note 所有由 client 创建的 awaitable 都必须在租约归还前完成。
 */
class AsyncEtcdClientLease
{
public:
    AsyncEtcdClientLease() noexcept = default;
    AsyncEtcdClientLease(AsyncEtcdClientLease&& other) noexcept;
    AsyncEtcdClientLease& operator=(AsyncEtcdClientLease&& other) noexcept;
    AsyncEtcdClientLease(const AsyncEtcdClientLease&) = delete;
    AsyncEtcdClientLease& operator=(const AsyncEtcdClientLease&) = delete;
    ~AsyncEtcdClientLease();

    /** @brief 获取租约持有的 client；空租约返回 nullptr。 */
    [[nodiscard]] AsyncEtcdClient* get() const noexcept;
    /** @brief 解引用租约持有的 client。 */
    [[nodiscard]] AsyncEtcdClient& operator*() const noexcept;
    /** @brief 访问租约持有的 client。 */
    [[nodiscard]] AsyncEtcdClient* operator->() const noexcept;
    /** @brief 判断租约是否持有 client。 */
    [[nodiscard]] explicit operator bool() const noexcept;
    /** @brief 提前归还 client；可重复调用且不阻塞。 */
    void release() noexcept;

private:
    friend class AsyncEtcdClusterClient;

    AsyncEtcdClientLease(
        std::shared_ptr<details::AsyncEtcdClientPoolState> state,
        AsyncEtcdClient* client) noexcept;

    std::shared_ptr<details::AsyncEtcdClientPoolState> m_state;
    AsyncEtcdClient* m_client = nullptr;
};

using AsyncEtcdClientAcquireResult = std::expected<AsyncEtcdClientLease, EtcdError>;

/**
 * @brief 多端点 AsyncEtcdClient 无锁池
 * @details 为每个 endpoint 创建固定数量且绑定同一 IOScheduler 的 AsyncEtcdClient。
 *          调用方通过 tryAcquire() 获取独占租约，再直接 co_await client 操作；池本身
 *          不执行连接、请求、重试或健康检查。
 * @note tryAcquire() 不挂起、不阻塞；池空时返回 EtcdErrorType::PoolExhausted。
 */
class AsyncEtcdClusterClient
{
public:
    explicit AsyncEtcdClusterClient(galay::kernel::IOScheduler* scheduler = nullptr,
                                    EtcdConfig config = {});

private:
    AsyncEtcdClusterClient(const AsyncEtcdClusterClient&) = delete;
    AsyncEtcdClusterClient& operator=(const AsyncEtcdClusterClient&) = delete;
public:
    AsyncEtcdClusterClient(AsyncEtcdClusterClient&&) noexcept = default;
    AsyncEtcdClusterClient& operator=(AsyncEtcdClusterClient&&) noexcept = default;
    ~AsyncEtcdClusterClient() = default;

    /**
     * @brief 尝试获取一个独占 AsyncEtcdClient 租约
     * @return 成功返回租约；配置无效、池内部失败或暂无空闲 client 时返回 EtcdError
     * @note 该操作只访问无锁队列，不挂起协程，也不执行网络连接。
     */
    [[nodiscard]] AsyncEtcdClientAcquireResult tryAcquire();

    /**
     * @brief 异步获取独占租约并确保 client 已连接
     * @return Task 完成后返回已连接租约，或返回池获取、建连对应的 EtcdError
     * @note 建连由池绑定的 IOScheduler 挂起推进，不阻塞调用线程；错误路径自动归还租约。
     */
    [[nodiscard]] galay::kernel::Task<AsyncEtcdClientAcquireResult> acquireConnected();

    /**
     * @brief 使用一个已连接的独占 client 执行异步操作
     * @tparam Fn 可调用对象类型，签名为 `Task<std::expected<T, EtcdError>>(AsyncEtcdClient&)`
     * @param fn 按值保存到协程 frame 的异步操作，支持移动传入
     * @return Task 完成后返回操作结果；Task 调度/消费错误映射为 Internal
     * @note 方法只通过 co_await 挂起，不阻塞线程；租约会跨所有挂起点存活，并在任意返回路径自动归还。
     */
    template <class Fn>
    [[nodiscard]] auto withClient(Fn fn) -> galay::kernel::Task<
        std::expected<details::AsyncEtcdOperationValue<Fn>, EtcdError>>;

    /** @brief 返回池持有的 client 总数。 */
    [[nodiscard]] size_t size() const noexcept;
    /** @brief 返回当前空闲 client 数量的并发快照。 */
    [[nodiscard]] size_t idleCount() const noexcept;
    /** @brief 返回池内 AsyncEtcdClient 固定绑定的 IOScheduler。 */
    [[nodiscard]] galay::kernel::IOScheduler* scheduler() const noexcept;

private:
    std::shared_ptr<details::AsyncEtcdClientPoolState> m_state;
};

/**
 * @brief AsyncEtcdClient 无锁池构建器
 */
class AsyncEtcdClusterClientBuilder
{
public:
    AsyncEtcdClusterClientBuilder() = default;
    AsyncEtcdClusterClientBuilder(AsyncEtcdClusterClientBuilder&&) noexcept = default;
    AsyncEtcdClusterClientBuilder& operator=(AsyncEtcdClusterClientBuilder&&) noexcept = default;

    AsyncEtcdClusterClientBuilder& scheduler(galay::kernel::IOScheduler* scheduler)
    {
        m_scheduler = scheduler;
        return *this;
    }

    AsyncEtcdClusterClientBuilder& endpoint(std::string endpoint)
    {
        m_config.endpoint = std::move(endpoint);
        return *this;
    }

    AsyncEtcdClusterClientBuilder& apiPrefix(std::string prefix)
    {
        m_config.api_prefix = std::move(prefix);
        return *this;
    }

    AsyncEtcdClusterClientBuilder& productionConfig(EtcdProductionConfig config)
    {
        if (!config.endpoints.empty()) {
            m_config.endpoint = config.endpoints.front();
        }
        m_config.production = std::move(config);
        return *this;
    }

    AsyncEtcdClusterClientBuilder& connectionsPerEndpoint(size_t count)
    {
        m_config.production.connections_per_endpoint = count;
        return *this;
    }

    AsyncEtcdClusterClientBuilder& requestTimeout(std::chrono::milliseconds timeout)
    {
        m_config.request_timeout = timeout;
        return *this;
    }

    AsyncEtcdClusterClientBuilder& bufferSize(size_t size)
    {
        m_config.buffer_size = size;
        return *this;
    }

    AsyncEtcdClusterClientBuilder& keepAlive(bool enabled)
    {
        m_config.keepalive = enabled;
        return *this;
    }

    AsyncEtcdClusterClientBuilder& config(EtcdConfig config)
    {
        m_config = std::move(config);
        return *this;
    }

    AsyncEtcdClusterClient build() const;

    /**
     * @brief 显式复制 builder 配置状态
     * @return 当前 builder 的独立副本
     */
    [[nodiscard]] AsyncEtcdClusterClientBuilder clone() const
    {
        return AsyncEtcdClusterClientBuilder(*this);
    }

    EtcdConfig& buildConfig()
    {
        return m_config;
    }

    const EtcdConfig& buildConfig() const
    {
        return m_config;
    }

private:
    AsyncEtcdClusterClientBuilder(const AsyncEtcdClusterClientBuilder&) = default;
    AsyncEtcdClusterClientBuilder& operator=(const AsyncEtcdClusterClientBuilder&) = default;

    EtcdConfig m_config{};
    galay::kernel::IOScheduler* m_scheduler = nullptr;
};

} // namespace galay::etcd

#include "../details/awaitable.h"

template <class Fn>
auto galay::etcd::AsyncEtcdClusterClient::withClient(Fn fn) -> galay::kernel::Task<
    std::expected<details::AsyncEtcdOperationValue<Fn>, EtcdError>>
{
    auto lease_result = tryAcquire();
    if (!lease_result.has_value()) {
        co_return std::unexpected(lease_result.error());
    }

    auto lease = std::move(*lease_result);
    if (!lease->connected()) {
        auto connected = co_await lease->connect();
        if (!connected.has_value()) {
            co_return std::unexpected(connected.error());
        }
    }

    auto operation_task_result = co_await std::invoke(fn, *lease);
    if (!operation_task_result.has_value()) {
        co_return std::unexpected(EtcdError(
            EtcdErrorType::Internal,
            std::string(operation_task_result.error().message())));
    }
    co_return std::move(operation_task_result.value());
}

inline galay::etcd::AsyncEtcdClient galay::etcd::AsyncEtcdClientBuilder::build() const
{
    return AsyncEtcdClient(m_scheduler, m_config);
}

inline galay::etcd::AsyncEtcdClusterClient galay::etcd::AsyncEtcdClusterClientBuilder::build() const
{
    return AsyncEtcdClusterClient(m_scheduler, m_config);
}

#endif // GALAY_ETCD_ASYNC_ETCD_CLIENT_H
