/**
 * @file rpc_config.h
 * @brief RPC运行时配置快照与本地配置 provider
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供版本化不可变配置快照。读取路径基于 shared_ptr 原子操作，不使用互斥锁，
 *          便于后续 RpcChannel 在每次调用时低开销读取 timeout、流控和路由策略。
 */

#ifndef GALAY_RPC_CONFIG_H
#define GALAY_RPC_CONFIG_H

#include "rpc_policy.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace galay::rpc
{

/**
 * @brief RPC配置校验错误码。
 */
enum class RpcConfigError {
    kInvalidMaxOutstanding, ///< max_outstanding_requests 必须大于 0。
    kInvalidMaxMessageSize, ///< max_message_size 必须大于 0 且不超过合理上限。
    kInvalidTimeout,        ///< timeout/deadline 必须在允许范围内。
    kInvalidRoutePolicy,    ///< 路由策略 key 或字段无效。
};

constexpr std::uint32_t kRpcConfigMaxMessageSize = 512U * 1024U * 1024U;
constexpr int kRpcConfigMaxTimeoutMs = 24 * 60 * 60 * 1000;

/**
 * @brief RPC客户端基础配置。
 */
struct RpcClientRuntimeConfig {
    std::uint32_t max_outstanding_requests = 1024; ///< 连接级最大 outstanding 请求数，必须大于 0。
    std::uint32_t max_message_size = RPC_MAX_BODY_SIZE; ///< 最大消息体字节数，必须大于 0。
    int connect_timeout_ms = 5000;                 ///< 建连超时，毫秒，0 表示不设置超时。
    int idle_timeout_ms = 60000;                   ///< 空闲超时，毫秒，0 表示不设置超时。
};

/**
 * @brief RPC服务端基础配置。
 */
struct RpcServerRuntimeConfig {
    std::uint32_t max_connections = 4096;          ///< 最大连接数，0 表示不限制。
    std::uint32_t max_outstanding_requests = 4096; ///< 服务端最大 outstanding 请求数，必须大于 0。
    std::uint32_t max_message_size = RPC_MAX_BODY_SIZE; ///< 最大消息体字节数，必须大于 0。
    int request_timeout_ms = 30000;                ///< 服务端请求处理超时，毫秒，0 表示不设置超时。
};

/**
 * @brief RPC流控配置。
 */
struct RpcFlowControlConfig {
    std::uint32_t max_inflight_requests = 1024;    ///< 最大飞行中请求数，必须大于 0。
    std::uint32_t max_inflight_bytes = RPC_MAX_BODY_SIZE * 4U; ///< 最大飞行中字节数，必须大于 0。
    std::uint32_t write_queue_high_watermark = 1024; ///< 写队列高水位，必须大于 0。
};

/**
 * @brief RPC timeout/deadline 配置。
 */
struct RpcDeadlineConfig {
    int default_timeout_ms = 30000; ///< 默认单次调用超时，毫秒，0 表示不设置超时。
    int default_deadline_ms = 30000; ///< 默认总 deadline，毫秒，0 表示不设置 deadline。
};

/**
 * @brief RPC运行时配置。
 *
 * @details provider 发布时会复制该值并写入单调递增 version。发布后的
 *          `std::shared_ptr<const RpcRuntimeConfig>` 视为不可变快照。
 */
struct RpcRuntimeConfig {
    std::uint64_t version = 0;                  ///< 快照版本，由 provider 分配。
    RpcClientRuntimeConfig client;              ///< 客户端基础配置。
    RpcServerRuntimeConfig server;              ///< 服务端基础配置。
    RpcFlowControlConfig flow_control;          ///< 流控基础配置。
    RpcDeadlineConfig deadline;                 ///< timeout/deadline 基础配置。
    RpcRoutePolicy default_route_policy;        ///< 路由缺省策略。
    RpcRoutePolicyMap route_policies;           ///< service/method/mode 精确策略表。
};

using RpcRuntimeConfigSnapshot = std::shared_ptr<const RpcRuntimeConfig>;

/**
 * @brief 将配置错误码转换为稳定文本。
 */
inline const char* rpcConfigErrorToString(RpcConfigError error) noexcept
{
    switch (error) {
        case RpcConfigError::kInvalidMaxOutstanding:
            return "invalid max outstanding";
        case RpcConfigError::kInvalidMaxMessageSize:
            return "invalid max message size";
        case RpcConfigError::kInvalidTimeout:
            return "invalid timeout";
        case RpcConfigError::kInvalidRoutePolicy:
            return "invalid route policy";
        default:
            return "unknown config error";
    }
}

/**
 * @brief 校验 timeout/deadline 字段。
 * @return 成功返回空 expected；失败返回 RpcConfigError。
 * @note 仅做 CPU 内存校验，不阻塞，不抛出异常。
 */
inline std::expected<void, RpcConfigError> validateRpcTimeoutMs(int value)
{
    if (value < 0 || value > kRpcConfigMaxTimeoutMs) {
        return std::unexpected(RpcConfigError::kInvalidTimeout);
    }
    return {};
}

/**
 * @brief 校验运行时配置。
 * @param config 待发布配置值。
 * @return 成功返回空 expected；失败返回首个命中的 RpcConfigError。
 * @note 不使用异常作为普通错误通道；调用方可据此拒绝发布并保留旧快照。
 */
inline std::expected<void, RpcConfigError> validateRpcRuntimeConfig(const RpcRuntimeConfig& config)
{
    if (config.client.max_outstanding_requests == 0 ||
        config.server.max_outstanding_requests == 0 ||
        config.flow_control.max_inflight_requests == 0 ||
        config.default_route_policy.max_outstanding_requests == 0) {
        return std::unexpected(RpcConfigError::kInvalidMaxOutstanding);
    }

    if (config.client.max_message_size == 0 ||
        config.server.max_message_size == 0 ||
        config.default_route_policy.max_message_size == 0 ||
        config.client.max_message_size > kRpcConfigMaxMessageSize ||
        config.server.max_message_size > kRpcConfigMaxMessageSize ||
        config.default_route_policy.max_message_size > kRpcConfigMaxMessageSize ||
        config.flow_control.max_inflight_bytes == 0) {
        return std::unexpected(RpcConfigError::kInvalidMaxMessageSize);
    }

    const int timeouts[] = {
        config.client.connect_timeout_ms,
        config.client.idle_timeout_ms,
        config.server.request_timeout_ms,
        config.deadline.default_timeout_ms,
        config.deadline.default_deadline_ms,
        config.default_route_policy.timeout_ms,
        config.default_route_policy.deadline_ms,
    };
    for (const int timeout : timeouts) {
        if (auto valid = validateRpcTimeoutMs(timeout); !valid.has_value()) {
            return valid;
        }
    }

    for (const auto& [key, policy] : config.route_policies) {
        if (key.service.empty() || key.method.empty()) {
            return std::unexpected(RpcConfigError::kInvalidRoutePolicy);
        }
        if (policy.max_outstanding_requests == 0) {
            return std::unexpected(RpcConfigError::kInvalidMaxOutstanding);
        }
        if (policy.max_message_size == 0 || policy.max_message_size > kRpcConfigMaxMessageSize) {
            return std::unexpected(RpcConfigError::kInvalidMaxMessageSize);
        }
        if (auto valid = validateRpcTimeoutMs(policy.timeout_ms); !valid.has_value()) {
            return valid;
        }
        if (auto valid = validateRpcTimeoutMs(policy.deadline_ms); !valid.has_value()) {
            return valid;
        }
    }

    return {};
}

/**
 * @brief 在运行时配置中查找路由策略，缺失时返回默认策略。
 * @note 返回引用生命周期绑定到 config；读取不阻塞。
 */
inline const RpcRoutePolicy& findRpcRoutePolicy(const RpcRuntimeConfig& config,
                                                std::string_view service,
                                                std::string_view method,
                                                RpcCallMode mode)
{
    return findRpcRoutePolicy(config.route_policies, config.default_route_policy,
                              service, method, mode);
}

/**
 * @brief 只读静态配置 provider。
 *
 * @details 构造时校验配置并保存不可变快照。后续 snapshot() 为 shared_ptr 原子读取，
 *          不使用 mutex，也不返回 Task<T>。
 */
class StaticRpcConfigProvider {
public:
    explicit StaticRpcConfigProvider(RpcRuntimeConfig config = {})
        : snapshot_(makeSnapshot(std::move(config), 1))
    {
    }

    StaticRpcConfigProvider(const StaticRpcConfigProvider&) = delete;
    StaticRpcConfigProvider& operator=(const StaticRpcConfigProvider&) = delete;

    StaticRpcConfigProvider(StaticRpcConfigProvider&& other) noexcept
        : snapshot_(std::atomic_load_explicit(&other.snapshot_, std::memory_order_acquire))
    {
    }

    StaticRpcConfigProvider& operator=(StaticRpcConfigProvider&& other) noexcept
    {
        if (this != &other) {
            auto snapshot = std::atomic_load_explicit(&other.snapshot_, std::memory_order_acquire);
            std::atomic_store_explicit(&snapshot_, snapshot, std::memory_order_release);
        }
        return *this;
    }

    /**
     * @brief 校验并创建静态 provider。
     * @return 成功返回 provider；失败返回 RpcConfigError，不抛异常。
     */
    static std::expected<StaticRpcConfigProvider, RpcConfigError> create(RpcRuntimeConfig config)
    {
        if (auto valid = validateRpcRuntimeConfig(config); !valid.has_value()) {
            return std::unexpected(valid.error());
        }
        return StaticRpcConfigProvider(makeSnapshot(std::move(config), 1));
    }

    /**
     * @brief 读取当前不可变快照。
     * @return 非空 `std::shared_ptr<const RpcRuntimeConfig>`。
     * @note shared_ptr 原子 load，无显式阻塞同步。
     */
    RpcRuntimeConfigSnapshot snapshot() const noexcept
    {
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    }

private:
    explicit StaticRpcConfigProvider(RpcRuntimeConfigSnapshot snapshot)
        : snapshot_(std::move(snapshot))
    {
    }

    static RpcRuntimeConfigSnapshot makeSnapshot(RpcRuntimeConfig config, std::uint64_t version)
    {
        config.version = version;
        return std::make_shared<const RpcRuntimeConfig>(std::move(config));
    }

    RpcRuntimeConfigSnapshot snapshot_;
};

/**
 * @brief 无阻塞 in-memory 配置 provider。
 *
 * @details publish() 校验新配置，通过后用 shared_ptr 原子 compare_exchange 发布新的不可变快照。
 *          snapshot() 读路径为 shared_ptr 原子 load；没有 mutex/condition_variable。
 */
class InMemoryRpcConfigProvider {
public:
    explicit InMemoryRpcConfigProvider(RpcRuntimeConfig initial = {})
        : snapshot_(makeSnapshot(std::move(initial), 1))
    {
    }

    /**
     * @brief 发布新配置快照。
     * @param config 新配置值，version 字段会被 provider 覆盖。
     * @return 成功返回新快照；校验失败返回 RpcConfigError，旧快照保持不变。
     * @note 发布路径不返回 Task<T>；并发发布通过 shared_ptr 原子 CAS 串行化版本。
     */
    std::expected<RpcRuntimeConfigSnapshot, RpcConfigError> publish(RpcRuntimeConfig config)
    {
        if (auto valid = validateRpcRuntimeConfig(config); !valid.has_value()) {
            return std::unexpected(valid.error());
        }

        auto current = std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
        for (;;) {
            const std::uint64_t next_version = current == nullptr ? 1 : current->version + 1;
            auto next = makeSnapshot(config, next_version);
            if (std::atomic_compare_exchange_weak_explicit(&snapshot_, &current, next,
                                                           std::memory_order_release,
                                                           std::memory_order_acquire)) {
                return next;
            }
        }
    }

    /**
     * @brief 读取当前不可变快照。
     * @return 非空 `std::shared_ptr<const RpcRuntimeConfig>`。
     * @note shared_ptr 原子 load，无显式阻塞同步。
     */
    RpcRuntimeConfigSnapshot snapshot() const noexcept
    {
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    }

private:
    static RpcRuntimeConfigSnapshot makeSnapshot(RpcRuntimeConfig config, std::uint64_t version)
    {
        config.version = version;
        return std::make_shared<const RpcRuntimeConfig>(std::move(config));
    }

    RpcRuntimeConfigSnapshot snapshot_;
};

} // namespace galay::rpc

#endif // GALAY_RPC_CONFIG_H
