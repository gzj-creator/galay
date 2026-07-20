/**
 * @file t12_cluster_policy.cc
 * @brief 用途：锁定 ETCD cluster policy、health snapshot 与 retry/backoff 的离线行为。
 */

#include <galay/cpp/galay-etcd/cluster/etcd_cluster_client.h>

#include <chrono>
#include <iostream>
#include <string>

using galay::etcd::EtcdClientStats;
using galay::etcd::EtcdClusterState;
using galay::etcd::EtcdEndpointHealthState;
using galay::etcd::EtcdEndpointPolicy;
using galay::etcd::EtcdError;
using galay::etcd::EtcdErrorType;
using galay::etcd::EtcdProductionConfig;
using galay::etcd::EtcdRetryDecision;

namespace
{

int fail(const std::string& message)
{
    std::cerr << "[FAIL] " << message << '\n';
    return 1;
}

} // namespace

int main()
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

    EtcdClusterState state(production);

    const auto first = state.selectEndpoint();
    const auto second = state.selectEndpoint();
    const auto third = state.selectEndpoint();
    if (!first.has_value() || !second.has_value() || !third.has_value()) {
        return fail("selectEndpoint should return valid indexes");
    }
    if (*first != 0 || *second != 1 || *third != 2) {
        return fail("round robin selection order mismatch");
    }

    state.markFailure(
        0,
        EtcdError(EtcdErrorType::Connection, "dial failed"),
        true,
        std::chrono::system_clock::time_point(std::chrono::seconds(0)));
    const auto fourth = state.selectEndpoint();
    if (!fourth.has_value() || *fourth != 1) {
        return fail("unhealthy endpoint should be skipped by round robin");
    }

    state.markSuccess(1, std::chrono::system_clock::time_point(std::chrono::seconds(123)));
    const auto& snapshots = state.getEndpointSnapshots();
    if (snapshots.size() != 3) {
        return fail("snapshot size mismatch");
    }
    if (snapshots[0].state != EtcdEndpointHealthState::Unhealthy ||
        !snapshots[0].last_error.has_value() ||
        snapshots[0].last_error->type() != EtcdErrorType::Connection) {
        return fail("failure snapshot mismatch");
    }
    if (snapshots[1].state != EtcdEndpointHealthState::Healthy ||
        !snapshots[1].last_success_time.has_value()) {
        return fail("success snapshot mismatch");
    }
    if (snapshots[0].last_probe_time.has_value()) {
        return fail("passive request failure should not create probe timestamp");
    }

    if (state.classifyRetry(EtcdError(EtcdErrorType::Connection, "dial failed"), 0) !=
        EtcdRetryDecision::RetryNextEndpoint) {
        return fail("connection error should retry next endpoint");
    }
    if (state.classifyRetry(EtcdError(EtcdErrorType::Server, "server unavailable"), 1) !=
        EtcdRetryDecision::RetrySameEndpoint) {
        return fail("server error should retry same endpoint before limit");
    }
    if (state.classifyRetry(EtcdError(EtcdErrorType::InvalidParam, "bad key"), 0) !=
        EtcdRetryDecision::FailFast) {
        return fail("invalid param should fail fast");
    }
    if (state.classifyRetry(EtcdError(EtcdErrorType::Timeout, "timeout"), 4) !=
        EtcdRetryDecision::FailFast) {
        return fail("attempts beyond retry limit should fail fast");
    }

    if (state.backoffForAttempt(0) != std::chrono::milliseconds(10) ||
        state.backoffForAttempt(1) != std::chrono::milliseconds(20) ||
        state.backoffForAttempt(2) != std::chrono::milliseconds(40) ||
        state.backoffForAttempt(3) != std::chrono::milliseconds(40)) {
        return fail("backoff bounds mismatch");
    }

    const EtcdClientStats stats = state.getStats();
    if (stats.request_failures != 1 || stats.endpoint_switches != 1) {
        return fail("stats should track failure and endpoint switch");
    }

    const auto probe_before_interval = state.collectDueProbes(
        std::chrono::system_clock::time_point(std::chrono::seconds(4)));
    if (!probe_before_interval.empty()) {
        return fail("probe should not fire before health interval");
    }

    const auto probe_after_interval = state.collectDueProbes(
        std::chrono::system_clock::time_point(std::chrono::seconds(5)));
    if (probe_after_interval.size() != 1 || probe_after_interval.front() != 0) {
        return fail("probe should target unhealthy endpoint after health interval");
    }

    const auto probe_duplicate = state.collectDueProbes(
        std::chrono::system_clock::time_point(std::chrono::seconds(6)));
    if (!probe_duplicate.empty()) {
        return fail("probe should not repeat before next interval window");
    }

    state.markProbeFailure(
        0,
        EtcdError(EtcdErrorType::Timeout, "probe timeout"),
        std::chrono::system_clock::time_point(std::chrono::seconds(7)));
    if (!snapshots[0].last_probe_time.has_value() ||
        *snapshots[0].last_probe_time != std::chrono::system_clock::time_point(std::chrono::seconds(7)) ||
        !snapshots[0].last_error.has_value() ||
        snapshots[0].last_error->type() != EtcdErrorType::Timeout) {
        return fail("probe failure should refresh probe timestamp and last error");
    }

    const auto probe_after_failure = state.collectDueProbes(
        std::chrono::system_clock::time_point(std::chrono::seconds(12)));
    if (probe_after_failure.size() != 1 || probe_after_failure.front() != 0) {
        return fail("probe failure should schedule another probe after interval");
    }

    state.markProbeSuccess(
        0,
        std::chrono::system_clock::time_point(std::chrono::seconds(13)));
    if (snapshots[0].state != EtcdEndpointHealthState::Healthy ||
        !snapshots[0].last_success_time.has_value() ||
        *snapshots[0].last_success_time != std::chrono::system_clock::time_point(std::chrono::seconds(13)) ||
        !snapshots[0].last_probe_time.has_value() ||
        *snapshots[0].last_probe_time != std::chrono::system_clock::time_point(std::chrono::seconds(13)) ||
        snapshots[0].last_error.has_value()) {
        return fail("probe success should restore healthy snapshot");
    }

    EtcdProductionConfig first_healthy = production;
    first_healthy.endpoint_policy = EtcdEndpointPolicy::FirstHealthy;
    EtcdClusterState first_healthy_state(first_healthy);
    first_healthy_state.markFailure(
        0,
        EtcdError(EtcdErrorType::Connection, "down"),
        true,
        std::chrono::system_clock::time_point(std::chrono::seconds(0)));
    const auto first_healthy_pick = first_healthy_state.selectEndpoint();
    if (!first_healthy_pick.has_value() || *first_healthy_pick != 1) {
        return fail("first healthy policy should pick first non-unhealthy endpoint");
    }

    EtcdProductionConfig sticky_leader = production;
    sticky_leader.endpoint_policy = EtcdEndpointPolicy::StickyLeader;
    sticky_leader.prefer_leader = true;
    EtcdClusterState sticky_leader_state(sticky_leader);
    sticky_leader_state.markSuccess(
        2,
        std::chrono::system_clock::time_point(std::chrono::seconds(20)));
    const auto sticky_leader_pick = sticky_leader_state.selectEndpoint();
    if (!sticky_leader_pick.has_value() || *sticky_leader_pick != 2) {
        return fail("sticky leader policy should prefer known healthy leader endpoint");
    }

    sticky_leader_state.markFailure(
        2,
        EtcdError(EtcdErrorType::Connection, "leader down"),
        true,
        std::chrono::system_clock::time_point(std::chrono::seconds(21)));
    const auto sticky_leader_fallback = sticky_leader_state.selectEndpoint();
    if (!sticky_leader_fallback.has_value() || *sticky_leader_fallback == 2) {
        return fail("sticky leader policy should fall back when leader becomes unhealthy");
    }

    std::cout << "ETCD CLUSTER POLICY TEST PASSED\n";
    return 0;
}
