/**
 * @file etcd_cluster_client.h
 * @brief etcd 多端点同步 cluster client 与 health snapshot 定义
 * @author galay-etcd
 * @version 1.0.0
 */

#ifndef GALAY_ETCD_CLUSTER_CLIENT_H
#define GALAY_ETCD_CLUSTER_CLIENT_H

#include "../base/etcd_config.h"
#include "../base/etcd_error.h"
#include "../base/etcd_types.h"
#include "../sync/etcd_client.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::etcd
{

/**
 * @brief etcd 端点健康状态
 */
enum class EtcdEndpointHealthState
{
    Unknown,   ///< 尚未有请求结果
    Healthy,   ///< 最近一次请求成功
    Unhealthy, ///< 最近一次网络级失败，当前被抑制
};

/**
 * @brief 单个 etcd 端点的健康快照
 */
struct EtcdEndpointHealthSnapshot
{
    std::string endpoint;                                                ///< 端点地址
    std::optional<EtcdError> last_error = std::nullopt;                  ///< 最近一次错误
    std::optional<std::chrono::system_clock::time_point> last_failure_time = std::nullopt; ///< 最近失败时间
    std::optional<std::chrono::system_clock::time_point> last_success_time = std::nullopt; ///< 最近成功时间
    std::optional<std::chrono::system_clock::time_point> last_probe_time = std::nullopt; ///< 最近主动探测时间
    size_t consecutive_failures = 0;                                     ///< 连续失败次数
    EtcdEndpointHealthState state = EtcdEndpointHealthState::Unknown;    ///< 当前健康状态
};

/**
 * @brief 管理 cluster policy、health snapshot 与统计快照
 * @details 该类不执行网络请求，只负责端点选择、失败抑制、重试分类与统计累计。
 */
class EtcdClusterState
{
public:
    explicit EtcdClusterState(EtcdProductionConfig production);
    EtcdClusterState(EtcdClusterState&&) noexcept = default;
    EtcdClusterState& operator=(EtcdClusterState&&) noexcept = default;

    /**
     * @brief 显式复制 cluster policy、health snapshot 与统计状态
     * @return 当前状态的独立副本
     */
    [[nodiscard]] EtcdClusterState clone() const
    {
        return EtcdClusterState(*this);
    }

    /**
     * @brief 选择下一次请求使用的端点下标
     * @return 成功返回端点下标；没有可用端点时返回空
     */
    [[nodiscard]] std::optional<size_t> selectEndpoint();

    /**
     * @brief 记录一次请求开始
     */
    void recordRequest();

    /**
     * @brief 记录一次重试
     */
    void recordRetry();

    /**
     * @brief 记录端点成功
     * @param index 端点下标
     * @param when 成功时间，默认使用当前系统时间
     */
    void markSuccess(
        size_t index,
        std::chrono::system_clock::time_point when = std::chrono::system_clock::now());

    /**
     * @brief 记录端点失败
     * @param index 端点下标
     * @param error 失败错误
     * @param endpoint_unhealthy 是否将该端点标记为 unhealthy
     */
    void markFailure(
        size_t index,
        EtcdError error,
        bool endpoint_unhealthy = true,
        std::chrono::system_clock::time_point when = std::chrono::system_clock::now());

    /**
     * @brief 收集当前到期的 unhealthy 端点探测任务
     * @param now 当前时间
     * @return 需要主动探测的端点下标列表
     * @note 该方法会在返回前记录本次 probe 时间，避免同一窗口重复探测。
     */
    [[nodiscard]] std::vector<size_t> collectDueProbes(
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    /**
     * @brief 记录一次主动探测成功
     * @param index 端点下标
     * @param when 探测成功时间
     */
    void markProbeSuccess(
        size_t index,
        std::chrono::system_clock::time_point when = std::chrono::system_clock::now());

    /**
     * @brief 记录一次主动探测失败
     * @param index 端点下标
     * @param error 探测失败错误
     * @param when 探测失败时间
     */
    void markProbeFailure(
        size_t index,
        EtcdError error,
        std::chrono::system_clock::time_point when = std::chrono::system_clock::now());

    /**
     * @brief 将错误分类为重试动作
     * @param error 失败错误
     * @param attempt 已发生的尝试次数（从 0 开始）
     * @return 下一步重试动作
     */
    [[nodiscard]] EtcdRetryDecision classifyRetry(const EtcdError& error, size_t attempt) const;

    /**
     * @brief 计算某次重试前的退避时间
     * @param attempt 已发生的重试次数（从 0 开始）
     * @return 退避时间，按指数增长并受 max_backoff 限制
     * @note 当前不在此处引入随机抖动，保持离线策略测试可重复。
     */
    [[nodiscard]] std::chrono::milliseconds backoffForAttempt(size_t attempt) const;

    /**
     * @brief 获取端点健康快照
     * @return 只读端点快照列表
     */
    [[nodiscard]] const std::vector<EtcdEndpointHealthSnapshot>& getEndpointSnapshots() const;

    /**
     * @brief 获取统计快照
     * @return 当前统计副本
     */
    [[nodiscard]] EtcdClientStats getStats() const;

    /**
     * @brief 获取当前有效重试次数
     * @return 至少为 1 的尝试次数
     */
    [[nodiscard]] size_t maxAttempts() const;

private:
    EtcdClusterState(const EtcdClusterState&) = default;
    EtcdClusterState& operator=(const EtcdClusterState&) = default;

    [[nodiscard]] std::optional<size_t> selectStickyLeaderEndpoint() const;
    [[nodiscard]] bool hasAlternativeEndpoint(size_t excluded_index) const;

private:
    EtcdProductionConfig m_production;
    std::vector<EtcdEndpointHealthSnapshot> m_snapshots;
    EtcdClientStats m_stats{};
    size_t m_round_robin_cursor = 0;
    std::optional<size_t> m_leader_hint = std::nullopt;
};

namespace details
{
struct EtcdClientPoolState;
} // namespace details

class EtcdClusterClient;

/**
 * @brief 同步 EtcdClient 池租约
 * @details move-only RAII 句柄，保证同一个 EtcdClient 同时只由一个调用方使用。
 *          租约析构或调用 release() 时会把 client 归还无锁空闲队列。
 */
class EtcdClientLease
{
public:
    EtcdClientLease() noexcept = default;
    EtcdClientLease(EtcdClientLease&& other) noexcept;
    EtcdClientLease& operator=(EtcdClientLease&& other) noexcept;
    EtcdClientLease(const EtcdClientLease&) = delete;
    EtcdClientLease& operator=(const EtcdClientLease&) = delete;
    ~EtcdClientLease();

    /** @brief 获取租约持有的 client；空租约返回 nullptr。 */
    [[nodiscard]] EtcdClient* get() const noexcept;
    /** @brief 解引用租约持有的 client。 */
    [[nodiscard]] EtcdClient& operator*() const noexcept;
    /** @brief 访问租约持有的 client。 */
    [[nodiscard]] EtcdClient* operator->() const noexcept;
    /** @brief 判断租约是否持有 client。 */
    [[nodiscard]] explicit operator bool() const noexcept;
    /** @brief 提前归还 client；可重复调用且不阻塞。 */
    void release() noexcept;

private:
    friend class EtcdClusterClient;

    EtcdClientLease(
        std::shared_ptr<details::EtcdClientPoolState> state,
        EtcdClient* client) noexcept;

    std::shared_ptr<details::EtcdClientPoolState> m_state;
    EtcdClient* m_client = nullptr;
};

using EtcdClientAcquireResult = std::expected<EtcdClientLease, EtcdError>;

/**
 * @brief ETCD cluster client 构建器
 */
class EtcdClusterClientBuilder
{
public:
    EtcdClusterClientBuilder() = default;
    EtcdClusterClientBuilder(EtcdClusterClientBuilder&&) noexcept = default;
    EtcdClusterClientBuilder& operator=(EtcdClusterClientBuilder&&) noexcept = default;

    EtcdClusterClientBuilder& endpoint(std::string endpoint);
    EtcdClusterClientBuilder& apiPrefix(std::string prefix);
    EtcdClusterClientBuilder& requestTimeout(std::chrono::milliseconds timeout);
    EtcdClusterClientBuilder& productionConfig(EtcdProductionConfig config);
    EtcdClusterClientBuilder& connectionsPerEndpoint(size_t count);
    EtcdClusterClientBuilder& config(EtcdConfig config);

    /**
     * @brief 显式复制 builder 配置状态
     * @return 当前 builder 的独立副本
     */
    [[nodiscard]] EtcdClusterClientBuilder clone() const
    {
        return EtcdClusterClientBuilder(*this);
    }

    [[nodiscard]] EtcdClusterClient build() const;
    [[nodiscard]] const EtcdConfig& buildConfig() const;

private:
    EtcdClusterClientBuilder(const EtcdClusterClientBuilder&) = default;
    EtcdClusterClientBuilder& operator=(const EtcdClusterClientBuilder&) = default;

    EtcdConfig m_config{};
};

/**
 * @brief 多端点同步 EtcdClient 无锁池
 * @details 为每个 endpoint 创建固定数量的 EtcdClient。调用方通过 tryAcquire()
 *          获取独占租约并直接执行 connect/put/get 等操作；池本身不代替调用方执行请求、
 *          重试或健康检查。
 * @note tryAcquire() 不阻塞；池空时返回 EtcdErrorType::PoolExhausted。
 */
class EtcdClusterClient
{
public:
    explicit EtcdClusterClient(EtcdConfig config = {});

    EtcdClusterClient(const EtcdClusterClient&) = delete;
    EtcdClusterClient& operator=(const EtcdClusterClient&) = delete;
    EtcdClusterClient(EtcdClusterClient&&) noexcept = default;
    EtcdClusterClient& operator=(EtcdClusterClient&&) noexcept = default;
    ~EtcdClusterClient() = default;

    /**
     * @brief 尝试获取一个独占 EtcdClient 租约
     * @return 成功返回租约；配置无效、池内部失败或暂无空闲 client 时返回 EtcdError
     * @note 该操作只访问无锁队列，不连接网络且不阻塞调用线程。
     */
    [[nodiscard]] EtcdClientAcquireResult tryAcquire();

    /**
     * @brief 获取独占租约并确保 client 已连接
     * @return 成功返回已连接租约；池获取或同步建连失败时返回 EtcdError
     * @note 该方法会执行同步网络连接，租约在错误路径上自动归还。
     */
    [[nodiscard]] EtcdClientAcquireResult acquireConnected();

    /**
     * @brief 使用一个已连接的独占 client 执行同步操作
     * @tparam Fn 可调用对象类型，签名为 `std::expected<T, EtcdError>(EtcdClient&)`
     * @param fn 要执行的同步操作
     * @return 操作结果；池获取、建连或操作失败均通过 EtcdError 返回
     * @note 租约在成功和所有错误路径上都会自动归还；该方法可能同步阻塞。
     */
    template <class Fn>
    [[nodiscard]] auto withClient(Fn&& fn)
        -> std::expected<
            typename std::invoke_result_t<Fn, EtcdClient&>::value_type,
            EtcdError>
    {
        auto lease = acquireConnected();
        if (!lease.has_value()) {
            return std::unexpected(lease.error());
        }
        return std::invoke(std::forward<Fn>(fn), **lease);
    }

    /** @brief 返回池持有的 client 总数。 */
    [[nodiscard]] size_t size() const noexcept;
    /** @brief 返回当前空闲 client 数量的并发快照。 */
    [[nodiscard]] size_t idleCount() const noexcept;

private:
    std::shared_ptr<details::EtcdClientPoolState> m_state;
};

} // namespace galay::etcd

inline galay::etcd::EtcdClusterClient galay::etcd::EtcdClusterClientBuilder::build() const
{
    return EtcdClusterClient(m_config);
}

inline const galay::etcd::EtcdConfig& galay::etcd::EtcdClusterClientBuilder::buildConfig() const
{
    return m_config;
}

#endif // GALAY_ETCD_CLUSTER_CLIENT_H
