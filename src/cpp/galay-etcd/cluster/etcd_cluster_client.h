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
#include <optional>
#include <span>
#include <string>
#include <thread>
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
    EtcdEndpointHealthState state = EtcdEndpointHealthState::Unknown;    ///< 当前健康状态
    std::optional<EtcdError> last_error = std::nullopt;                  ///< 最近一次错误
    std::optional<std::chrono::system_clock::time_point> last_failure_time = std::nullopt; ///< 最近失败时间
    std::optional<std::chrono::system_clock::time_point> last_success_time = std::nullopt; ///< 最近成功时间
    std::optional<std::chrono::system_clock::time_point> last_probe_time = std::nullopt; ///< 最近主动探测时间
    size_t consecutive_failures = 0;                                     ///< 连续失败次数
};

/**
 * @brief 管理 cluster policy、health snapshot 与统计快照
 * @details 该类不执行网络请求，只负责端点选择、失败抑制、重试分类与统计累计。
 */
class EtcdClusterState
{
public:
    explicit EtcdClusterState(EtcdProductionConfig production);

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
    [[nodiscard]] std::optional<size_t> selectStickyLeaderEndpoint() const;
    [[nodiscard]] bool hasAlternativeEndpoint(size_t excluded_index) const;

private:
    EtcdProductionConfig m_production;
    std::vector<EtcdEndpointHealthSnapshot> m_snapshots;
    EtcdClientStats m_stats{};
    size_t m_round_robin_cursor = 0;
    std::optional<size_t> m_leader_hint = std::nullopt;
};

class EtcdClusterClient;

/**
 * @brief ETCD cluster client 构建器
 */
class EtcdClusterClientBuilder
{
public:
    EtcdClusterClientBuilder& endpoint(std::string endpoint);
    EtcdClusterClientBuilder& apiPrefix(std::string prefix);
    EtcdClusterClientBuilder& requestTimeout(std::chrono::milliseconds timeout);
    EtcdClusterClientBuilder& productionConfig(EtcdProductionConfig config);
    EtcdClusterClientBuilder& config(EtcdConfig config);

    [[nodiscard]] EtcdClusterClient build() const;
    [[nodiscard]] const EtcdConfig& buildConfig() const;

private:
    EtcdConfig m_config{};
};

/**
 * @brief etcd 同步 cluster wrapper
 * @details 该类在每次请求时按 policy 选择 endpoint，并在网络级失败时切换端点重试。
 *          当前仅包装已有同步 `EtcdClient`，不引入后台健康检查线程。
 */
class EtcdClusterClient
{
public:
    using PipelineOp = galay::etcd::PipelineOp;

    explicit EtcdClusterClient(EtcdConfig config = {});

    EtcdClusterClient(const EtcdClusterClient&) = delete;
    EtcdClusterClient& operator=(const EtcdClusterClient&) = delete;
    EtcdClusterClient(EtcdClusterClient&&) = default;
    EtcdClusterClient& operator=(EtcdClusterClient&&) = default;
    ~EtcdClusterClient() = default;

    [[nodiscard]] EtcdBoolResult put(
        const std::string& key,
        const std::string& value,
        std::optional<int64_t> lease_id = std::nullopt);
    [[nodiscard]] EtcdGetResult get(
        const std::string& key,
        bool prefix = false,
        std::optional<int64_t> limit = std::nullopt);
    [[nodiscard]] EtcdDeleteResult del(const std::string& key, bool prefix = false);
    [[nodiscard]] EtcdLeaseGrantResult grantLease(int64_t ttl_seconds);
    [[nodiscard]] EtcdLeaseGrantResult keepAliveOnce(int64_t lease_id);
    [[nodiscard]] EtcdPipelineResult pipeline(std::span<const PipelineOp> operations);
    [[nodiscard]] EtcdPipelineResult pipeline(std::vector<PipelineOp> operations);

    [[nodiscard]] const std::vector<EtcdEndpointHealthSnapshot>& getEndpointSnapshots() const;
    [[nodiscard]] EtcdClientStats getStats() const;

private:
    [[nodiscard]] EtcdConfig configForEndpoint(size_t index) const;
    void runDueHealthProbes();

    template <typename Result, typename Operation>
    [[nodiscard]] Result execute(Operation&& operation)
    {
        runDueHealthProbes();
        m_state.recordRequest();

        std::optional<size_t> sticky_endpoint = std::nullopt;
        std::optional<EtcdError> last_error = std::nullopt;
        const size_t max_attempts = m_state.maxAttempts();

        for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
            const std::optional<size_t> selected = sticky_endpoint.has_value()
                ? sticky_endpoint
                : m_state.selectEndpoint();
            sticky_endpoint.reset();

            if (!selected.has_value()) {
                break;
            }

            EtcdClient client(configForEndpoint(*selected));
            auto connect_result = client.connect();
            if (!connect_result.has_value()) {
                last_error = connect_result.error();
            } else {
                Result result = operation(client);
                auto close_result = client.close();
                if (!close_result.has_value()) {
                    last_error = close_result.error();
                    const EtcdRetryDecision close_decision =
                        m_state.classifyRetry(*last_error, attempt);
                    m_state.markFailure(
                        *selected,
                        *last_error,
                        close_decision == EtcdRetryDecision::RetryNextEndpoint);
                    if (close_decision == EtcdRetryDecision::FailFast ||
                        attempt + 1 >= max_attempts) {
                        return std::unexpected(*last_error);
                    }
                    m_state.recordRetry();
                    if (close_decision == EtcdRetryDecision::RetrySameEndpoint) {
                        sticky_endpoint = *selected;
                    }
                    const auto backoff = m_state.backoffForAttempt(attempt);
                    if (backoff.count() > 0) {
                        std::this_thread::sleep_for(backoff);
                    }
                    continue;
                }
                if (result.has_value()) {
                    m_state.markSuccess(*selected);
                    return result;
                }
                last_error = result.error();
            }

            const EtcdRetryDecision decision = m_state.classifyRetry(*last_error, attempt);
            m_state.markFailure(*selected, *last_error, decision == EtcdRetryDecision::RetryNextEndpoint);
            if (decision == EtcdRetryDecision::FailFast || attempt + 1 >= max_attempts) {
                return std::unexpected(*last_error);
            }

            m_state.recordRetry();
            if (decision == EtcdRetryDecision::RetrySameEndpoint) {
                sticky_endpoint = *selected;
            }

            const auto backoff = m_state.backoffForAttempt(attempt);
            if (backoff.count() > 0) {
                std::this_thread::sleep_for(backoff);
            }
        }

        if (last_error.has_value()) {
            return std::unexpected(*last_error);
        }
        return std::unexpected(EtcdError(EtcdErrorType::InvalidEndpoint, "cluster endpoints are empty"));
    }

private:
    EtcdConfig m_config;
    EtcdClusterState m_state;
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
