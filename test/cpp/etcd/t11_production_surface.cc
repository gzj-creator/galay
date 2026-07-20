/**
 * @file t11_production_surface.cc
 * @brief 用途：锁定 production config、stats 与敏感信息脱敏的公开 API surface。
 *
 * 关键覆盖点：
 * - production policy/config/stats 类型可被直接包含和默认构造
 * - sync/async builder 可接收 production config，且不改变既有 EtcdConfig 构建入口
 * - sync/async client 暴露只读 stats snapshot
 * - credential/debug 字符串不会泄漏 password 或 bearer token
 */

#include <galay/cpp/galay-etcd/async/client.h>
#include <galay/cpp/galay-etcd/cluster/etcd_cluster_client.h>
#include <galay/cpp/galay-etcd/sync/etcd_client.h>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

using galay::etcd::AsyncEtcdClient;
using galay::etcd::AsyncEtcdClientBuilder;
using galay::etcd::AsyncEtcdClusterClient;
using galay::etcd::AsyncEtcdClusterClientBuilder;
using galay::etcd::AsyncEtcdClientAcquireResult;
using galay::etcd::EtcdClient;
using galay::etcd::EtcdClientAcquireResult;
using galay::etcd::EtcdClientBuilder;
using galay::etcd::EtcdClusterClient;
using galay::etcd::EtcdClusterClientBuilder;
using galay::etcd::EtcdClientStats;
using galay::etcd::EtcdCredentialConfig;
using galay::etcd::EtcdEndpointPolicy;
using galay::etcd::EtcdProductionConfig;
using galay::etcd::EtcdRetryConfig;
using galay::etcd::EtcdRetryDecision;

namespace {

template <typename BuilderT>
concept AcceptsProductionConfig = requires(BuilderT builder, EtcdProductionConfig config) {
    { builder.productionConfig(config) } -> std::same_as<BuilderT&>;
    builder.buildConfig().production.endpoints;
};

template <typename ClientT>
concept HasStatsSnapshot = requires(const ClientT& client) {
    { client.getStats() } -> std::same_as<EtcdClientStats>;
};

template <typename PoolT, typename AcquireResultT>
concept HasPoolSurface = requires(PoolT& pool, const PoolT& const_pool) {
    { pool.tryAcquire() } -> std::same_as<AcquireResultT>;
    { const_pool.size() } -> std::same_as<size_t>;
    { const_pool.idleCount() } -> std::same_as<size_t>;
};

static_assert(std::is_enum_v<EtcdEndpointPolicy>);
static_assert(std::is_enum_v<EtcdRetryDecision>);
static_assert(std::is_default_constructible_v<EtcdRetryConfig>);
static_assert(std::is_default_constructible_v<EtcdProductionConfig>);
static_assert(std::is_default_constructible_v<EtcdClientStats>);
static_assert(AcceptsProductionConfig<EtcdClientBuilder>);
static_assert(AcceptsProductionConfig<AsyncEtcdClientBuilder>);
static_assert(AcceptsProductionConfig<EtcdClusterClientBuilder>);
static_assert(AcceptsProductionConfig<AsyncEtcdClusterClientBuilder>);
static_assert(HasStatsSnapshot<EtcdClient>);
static_assert(HasStatsSnapshot<AsyncEtcdClient>);
static_assert(HasPoolSurface<EtcdClusterClient, EtcdClientAcquireResult>);
static_assert(HasPoolSurface<AsyncEtcdClusterClient, AsyncEtcdClientAcquireResult>);

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    EtcdRetryConfig retry;
    if (retry.attempts != 3 ||
        retry.initial_backoff != std::chrono::milliseconds(25) ||
        retry.max_backoff != std::chrono::milliseconds(500) ||
        !retry.jitter) {
        return 1;
    }

    EtcdProductionConfig production;
    production.endpoints = {"http://127.0.0.1:2379", "http://127.0.0.1:22379"};
    production.endpoint_policy = EtcdEndpointPolicy::RoundRobin;
    production.prefer_leader = true;
    production.retry.attempts = 5;
    production.connections_per_endpoint = 2;

    const auto sync_config = EtcdClientBuilder()
        .productionConfig(production)
        .buildConfig();
    if (sync_config.production.endpoints.size() != 2 ||
        sync_config.production.retry.attempts != 5 ||
        sync_config.endpoint != "http://127.0.0.1:2379") {
        return 2;
    }

    const auto async_config = AsyncEtcdClientBuilder()
        .productionConfig(production)
        .buildConfig();
    if (async_config.production.endpoint_policy != EtcdEndpointPolicy::RoundRobin) {
        return 3;
    }

    const auto async_cluster_config = AsyncEtcdClusterClientBuilder()
        .productionConfig(production)
        .buildConfig();
    if (async_cluster_config.production.endpoints.size() != 2 ||
        async_cluster_config.production.connections_per_endpoint != 2 ||
        async_cluster_config.endpoint != "http://127.0.0.1:2379") {
        return 6;
    }

    const EtcdClientStats stats;
    if (stats.requests != 0 ||
        stats.request_failures != 0 ||
        stats.retries != 0 ||
        stats.endpoint_switches != 0 ||
        stats.auth_refreshes != 0 ||
        stats.watch_reconnects != 0 ||
        stats.watch_compactions != 0 ||
        stats.lease_keepalive_successes != 0 ||
        stats.lease_keepalive_failures != 0) {
        return 4;
    }

    EtcdCredentialConfig credentials;
    credentials.username = "root";
    credentials.password = "plain-password";
    credentials.bearer_token = "bearer-token";

    const std::string redacted = credentials.redactedString();
    if (!contains(redacted, "root") ||
        contains(redacted, "plain-password") ||
        contains(redacted, "bearer-token")) {
        return 5;
    }

    return 0;
}
