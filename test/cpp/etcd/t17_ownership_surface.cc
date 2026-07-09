/**
 * @file t17_ownership_surface.cc
 * @brief 用途：锁定 etcd 非平凡状态类型的 move-only 与显式 clone 表面。
 */

#include <galay/cpp/galay-etcd/async/client.h>
#include <galay/cpp/galay-etcd/cluster/etcd_cluster_client.h>
#include <galay/cpp/galay-etcd/sync/etcd_client.h>

#include <chrono>
#include <concepts>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

using galay::etcd::AsyncEtcdClient;
using galay::etcd::AsyncEtcdClientBuilder;
using galay::etcd::AsyncEtcdClusterAttempt;
using galay::etcd::AsyncEtcdClusterClient;
using galay::etcd::AsyncEtcdClusterClientBuilder;
using galay::etcd::EtcdClient;
using galay::etcd::EtcdClientBuilder;
using galay::etcd::EtcdClusterClient;
using galay::etcd::EtcdClusterClientBuilder;
using galay::etcd::EtcdClusterState;
using galay::etcd::EtcdEndpointHealthState;
using galay::etcd::EtcdEndpointPolicy;
using galay::etcd::EtcdError;
using galay::etcd::EtcdErrorType;
using galay::etcd::EtcdProductionConfig;
using galay::etcd::PipelineOp;
using galay::etcd::PipelineOpType;

namespace
{

template <typename T>
concept HasValueClone = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

template <typename T>
concept HasNoValueClone = !HasValueClone<T>;

int fail(const std::string& message)
{
    std::cerr << "[FAIL] " << message << '\n';
    return 1;
}

bool samePipelineOp(const PipelineOp& lhs, const PipelineOp& rhs)
{
    return lhs.type == rhs.type &&
        lhs.key == rhs.key &&
        lhs.value == rhs.value &&
        lhs.prefix == rhs.prefix &&
        lhs.limit == rhs.limit &&
        lhs.lease_id == rhs.lease_id;
}

EtcdProductionConfig makeProduction()
{
    EtcdProductionConfig production;
    production.endpoints = {
        "http://127.0.0.1:2379",
        "http://127.0.0.1:22379",
        "http://127.0.0.1:32379",
    };
    production.endpoint_policy = EtcdEndpointPolicy::RoundRobin;
    production.retry.attempts = 4;
    production.retry.initial_backoff = std::chrono::milliseconds(10);
    production.retry.max_backoff = std::chrono::milliseconds(40);
    production.retry.jitter = false;
    return production;
}

} // namespace

static_assert(!std::is_copy_constructible_v<PipelineOp>);
static_assert(!std::is_copy_assignable_v<PipelineOp>);
static_assert(std::is_nothrow_move_constructible_v<PipelineOp>);
static_assert(std::is_nothrow_move_assignable_v<PipelineOp>);
static_assert(HasValueClone<PipelineOp>);

static_assert(!std::is_copy_constructible_v<EtcdClusterState>);
static_assert(!std::is_copy_assignable_v<EtcdClusterState>);
static_assert(std::is_nothrow_move_constructible_v<EtcdClusterState>);
static_assert(std::is_nothrow_move_assignable_v<EtcdClusterState>);
static_assert(HasValueClone<EtcdClusterState>);

static_assert(!std::is_copy_constructible_v<AsyncEtcdClusterAttempt>);
static_assert(!std::is_copy_assignable_v<AsyncEtcdClusterAttempt>);
static_assert(std::is_nothrow_move_constructible_v<AsyncEtcdClusterAttempt>);
static_assert(std::is_nothrow_move_assignable_v<AsyncEtcdClusterAttempt>);
static_assert(HasValueClone<AsyncEtcdClusterAttempt>);

static_assert(!std::is_copy_constructible_v<AsyncEtcdClusterClient>);
static_assert(!std::is_copy_assignable_v<AsyncEtcdClusterClient>);
static_assert(std::is_nothrow_move_constructible_v<AsyncEtcdClusterClient>);
static_assert(std::is_nothrow_move_assignable_v<AsyncEtcdClusterClient>);
static_assert(HasNoValueClone<AsyncEtcdClusterClient>);

static_assert(!std::is_copy_constructible_v<EtcdClusterClient>);
static_assert(!std::is_copy_assignable_v<EtcdClusterClient>);
static_assert(std::is_nothrow_move_constructible_v<EtcdClusterClient>);
static_assert(std::is_nothrow_move_assignable_v<EtcdClusterClient>);
static_assert(HasNoValueClone<EtcdClusterClient>);

static_assert(!std::is_copy_constructible_v<EtcdClient>);
static_assert(!std::is_copy_assignable_v<EtcdClient>);
static_assert(!std::is_move_constructible_v<EtcdClient>);
static_assert(!std::is_move_assignable_v<EtcdClient>);
static_assert(HasNoValueClone<EtcdClient>);

static_assert(!std::is_copy_constructible_v<AsyncEtcdClient>);
static_assert(!std::is_copy_assignable_v<AsyncEtcdClient>);
static_assert(!std::is_move_constructible_v<AsyncEtcdClient>);
static_assert(!std::is_move_assignable_v<AsyncEtcdClient>);
static_assert(HasNoValueClone<AsyncEtcdClient>);

static_assert(!std::is_copy_constructible_v<EtcdClientBuilder>);
static_assert(!std::is_copy_assignable_v<EtcdClientBuilder>);
static_assert(std::is_nothrow_move_constructible_v<EtcdClientBuilder>);
static_assert(std::is_nothrow_move_assignable_v<EtcdClientBuilder>);
static_assert(HasValueClone<EtcdClientBuilder>);

static_assert(!std::is_copy_constructible_v<EtcdClusterClientBuilder>);
static_assert(!std::is_copy_assignable_v<EtcdClusterClientBuilder>);
static_assert(std::is_nothrow_move_constructible_v<EtcdClusterClientBuilder>);
static_assert(std::is_nothrow_move_assignable_v<EtcdClusterClientBuilder>);
static_assert(HasValueClone<EtcdClusterClientBuilder>);

static_assert(!std::is_copy_constructible_v<AsyncEtcdClientBuilder>);
static_assert(!std::is_copy_assignable_v<AsyncEtcdClientBuilder>);
static_assert(std::is_nothrow_move_constructible_v<AsyncEtcdClientBuilder>);
static_assert(std::is_nothrow_move_assignable_v<AsyncEtcdClientBuilder>);
static_assert(HasValueClone<AsyncEtcdClientBuilder>);

static_assert(!std::is_copy_constructible_v<AsyncEtcdClusterClientBuilder>);
static_assert(!std::is_copy_assignable_v<AsyncEtcdClusterClientBuilder>);
static_assert(std::is_nothrow_move_constructible_v<AsyncEtcdClusterClientBuilder>);
static_assert(std::is_nothrow_move_assignable_v<AsyncEtcdClusterClientBuilder>);
static_assert(HasValueClone<AsyncEtcdClusterClientBuilder>);

int main()
{
    PipelineOp put = PipelineOp::Put("alpha", "one", 42);
    PipelineOp put_clone = put.clone();
    if (!samePipelineOp(put, put_clone)) {
        return fail("PipelineOp clone should duplicate all fields");
    }
    put_clone.key = "beta";
    if (put.key != "alpha" || put_clone.key != "beta") {
        return fail("PipelineOp clone should own independent strings");
    }

    EtcdClusterState state(makeProduction());
    state.recordRequest();
    state.recordRetry();
    const auto first = state.selectEndpoint();
    const auto second = state.selectEndpoint();
    if (!first.has_value() || *first != 0 || !second.has_value() || *second != 1) {
        return fail("EtcdClusterState setup should advance round robin cursor");
    }
    state.markFailure(
        0,
        EtcdError(EtcdErrorType::Connection, "dial failed"),
        true,
        std::chrono::system_clock::time_point(std::chrono::seconds(7)));
    state.markSuccess(
        1,
        std::chrono::system_clock::time_point(std::chrono::seconds(9)));

    EtcdClusterState state_clone = state.clone();
    const auto clone_stats = state_clone.getStats();
    if (clone_stats.requests != 1 ||
        clone_stats.retries != 1 ||
        clone_stats.request_failures != 1 ||
        clone_stats.endpoint_switches != 1) {
        return fail("EtcdClusterState clone should preserve stats");
    }
    const auto& clone_snapshots = state_clone.getEndpointSnapshots();
    if (clone_snapshots.size() != 3 ||
        clone_snapshots[0].state != EtcdEndpointHealthState::Unhealthy ||
        clone_snapshots[1].state != EtcdEndpointHealthState::Healthy ||
        !clone_snapshots[0].last_error.has_value()) {
        return fail("EtcdClusterState clone should preserve endpoint snapshots");
    }
    const auto clone_next = state_clone.selectEndpoint();
    const auto original_next = state.selectEndpoint();
    if (!clone_next.has_value() || !original_next.has_value() ||
        *clone_next != *original_next) {
        return fail("EtcdClusterState clone should preserve selection cursor");
    }

    AsyncEtcdClusterAttempt attempt;
    attempt.endpoint_index = 2;
    attempt.attempt = 3;
    attempt.config.endpoint = "http://127.0.0.1:32379";
    attempt.backoff = std::chrono::milliseconds(40);
    AsyncEtcdClusterAttempt attempt_clone = attempt.clone();
    if (attempt_clone.endpoint_index != attempt.endpoint_index ||
        attempt_clone.attempt != attempt.attempt ||
        attempt_clone.config.endpoint != attempt.config.endpoint ||
        attempt_clone.backoff != attempt.backoff) {
        return fail("AsyncEtcdClusterAttempt clone should duplicate retry snapshot");
    }

    EtcdClientBuilder sync_builder;
    sync_builder.endpoint("http://127.0.0.1:2379")
        .apiPrefix("/v3")
        .requestTimeout(std::chrono::milliseconds(250))
        .bufferSize(4096)
        .keepAlive(false)
        .tcpNoDelay(false);
    EtcdClientBuilder sync_builder_clone = sync_builder.clone();
    if (sync_builder_clone.buildConfig().endpoint != "http://127.0.0.1:2379" ||
        sync_builder_clone.buildConfig().request_timeout != std::chrono::milliseconds(250) ||
        sync_builder_clone.buildConfig().buffer_size != 4096) {
        return fail("EtcdClientBuilder clone should preserve config");
    }

    EtcdClusterClientBuilder cluster_builder;
    cluster_builder.productionConfig(makeProduction())
        .requestTimeout(std::chrono::milliseconds(300));
    EtcdClusterClientBuilder cluster_builder_clone = cluster_builder.clone();
    if (cluster_builder_clone.buildConfig().production.endpoints.size() != 3 ||
        cluster_builder_clone.buildConfig().request_timeout != std::chrono::milliseconds(300)) {
        return fail("EtcdClusterClientBuilder clone should preserve production config");
    }

    AsyncEtcdClientBuilder async_builder;
    async_builder.endpoint("http://127.0.0.1:22379")
        .requestTimeout(std::chrono::milliseconds(350))
        .tcpNoDelay(false);
    AsyncEtcdClientBuilder async_builder_clone = async_builder.clone();
    if (async_builder_clone.buildConfig().endpoint != "http://127.0.0.1:22379" ||
        async_builder_clone.buildConfig().request_timeout != std::chrono::milliseconds(350)) {
        return fail("AsyncEtcdClientBuilder clone should preserve config");
    }

    AsyncEtcdClusterClientBuilder async_cluster_builder;
    async_cluster_builder.productionConfig(makeProduction())
        .requestTimeout(std::chrono::milliseconds(400));
    AsyncEtcdClusterClientBuilder async_cluster_builder_clone = async_cluster_builder.clone();
    if (async_cluster_builder_clone.buildConfig().production.endpoints.size() != 3 ||
        async_cluster_builder_clone.buildConfig().request_timeout != std::chrono::milliseconds(400)) {
        return fail("AsyncEtcdClusterClientBuilder clone should preserve production config");
    }

    AsyncEtcdClusterClient async_cluster = async_cluster_builder_clone.build();
    AsyncEtcdClusterClient moved_async_cluster(std::move(async_cluster));
    auto moved_attempt = moved_async_cluster.beginAttempt().await_resume();
    if (!moved_attempt.has_value() ||
        moved_attempt->endpoint_index != 0 ||
        moved_attempt->config.endpoint != "http://127.0.0.1:2379") {
        return fail("moved AsyncEtcdClusterClient should retain offline policy state");
    }

    std::cout << "ETCD OWNERSHIP SURFACE TEST PASSED\n";
    return 0;
}
