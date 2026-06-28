/**
 * @file t14_async_cluster_integration.cc
 * @brief 用途：验证 async cluster offline policy wrapper 的 gated integration surface。
 */

#include <galay/cpp/galay-etcd/async/client.h>

#include "integration_config.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using galay::etcd::AsyncEtcdClusterClient;
using galay::etcd::AsyncEtcdClusterClientBuilder;
using galay::etcd::EtcdEndpointHealthState;
using galay::etcd::EtcdEndpointPolicy;
using galay::etcd::EtcdError;
using galay::etcd::EtcdErrorType;
using galay::etcd::EtcdProductionConfig;

namespace
{

int fail(const std::string& message)
{
    std::cerr << "[FAIL] " << message << '\n';
    return 1;
}

std::vector<std::string> parseEndpoints(const char* raw)
{
    std::vector<std::string> endpoints;
    if (raw == nullptr) {
        return endpoints;
    }

    std::string current;
    for (const char* it = raw; *it != '\0'; ++it) {
        if (*it == ',') {
            if (!current.empty()) {
                endpoints.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(*it);
    }
    if (!current.empty()) {
        endpoints.push_back(current);
    }
    return endpoints;
}

} // namespace

int main()
{
    if (const int skip_code = etcd_test::requireIntegrationEnabledOrSkip("etcd.it.async_cluster");
        skip_code != 0) {
        return skip_code;
    }

    const auto endpoints = parseEndpoints(std::getenv("GALAY_ETCD_ENDPOINTS"));
    if (endpoints.size() < 2) {
        std::cout << "[SKIP] etcd.it.async_cluster requires GALAY_ETCD_ENDPOINTS with at least 2 endpoints\n";
        return etcd_test::kEtcdTestSkippedExitCode;
    }

    EtcdProductionConfig production;
    production.endpoints = endpoints;
    production.endpoint_policy = EtcdEndpointPolicy::RoundRobin;
    production.retry.attempts = 3;
    production.retry.initial_backoff = std::chrono::milliseconds(7);
    production.retry.max_backoff = std::chrono::milliseconds(21);
    production.retry.jitter = false;

    const auto timeout = std::chrono::milliseconds(123);
    AsyncEtcdClusterClient client = AsyncEtcdClusterClientBuilder()
        .productionConfig(production)
        .apiPrefix("/galay-it")
        .requestTimeout(timeout)
        .build();

    auto first_attempt_awaitable = client.beginAttempt();
    if (!first_attempt_awaitable.await_ready()) {
        return fail("beginAttempt should be immediately ready for offline policy wrapper");
    }

    auto first_attempt = first_attempt_awaitable.await_resume();
    if (!first_attempt.has_value()) {
        return fail("beginAttempt failed: " + first_attempt.error().message());
    }
    if (first_attempt->endpoint_index != 0 ||
        first_attempt->attempt != 0 ||
        first_attempt->config.endpoint != endpoints[0] ||
        first_attempt->config.api_prefix != "/galay-it" ||
        first_attempt->config.request_timeout != timeout ||
        first_attempt->backoff != std::chrono::milliseconds::zero()) {
        return fail("first attempt should inject builder endpoint/api_prefix/timeout into config");
    }

    auto second_attempt = client.nextAttempt(
        *first_attempt,
        EtcdError(EtcdErrorType::Connection, "dial failed")).await_resume();
    if (!second_attempt.has_value()) {
        return fail("connection retry failed: " + second_attempt.error().message());
    }
    if (second_attempt->endpoint_index != 1 ||
        second_attempt->attempt != 1 ||
        second_attempt->config.endpoint != endpoints[1] ||
        second_attempt->config.api_prefix != "/galay-it" ||
        second_attempt->config.request_timeout != timeout ||
        second_attempt->backoff != std::chrono::milliseconds(7)) {
        return fail("connection retry should advance to the next configured endpoint");
    }

    auto third_attempt = client.nextAttempt(
        *second_attempt,
        EtcdError(EtcdErrorType::Server, "server unavailable")).await_resume();
    if (!third_attempt.has_value()) {
        return fail("server retry failed: " + third_attempt.error().message());
    }
    if (third_attempt->endpoint_index != 1 ||
        third_attempt->attempt != 2 ||
        third_attempt->config.endpoint != endpoints[1] ||
        third_attempt->config.api_prefix != "/galay-it" ||
        third_attempt->config.request_timeout != timeout ||
        third_attempt->backoff != std::chrono::milliseconds(14)) {
        return fail("server retry should stay on the same selected endpoint");
    }

    client.markSuccess(*third_attempt);
    const auto& snapshots = client.getEndpointSnapshots();
    if (snapshots.size() != endpoints.size() ||
        snapshots[0].state != EtcdEndpointHealthState::Unhealthy ||
        snapshots[1].state != EtcdEndpointHealthState::Healthy) {
        return fail("offline async cluster snapshots should reflect retry outcomes");
    }

    const auto stats = client.getStats();
    if (stats.requests != 1 ||
        stats.retries != 2 ||
        stats.request_failures != 2 ||
        stats.endpoint_switches != 1) {
        return fail("offline async cluster stats should reflect retry loop");
    }

    std::cout << "ETCD ASYNC CLUSTER INTEGRATION TEST PASSED\n";
    return 0;
}
