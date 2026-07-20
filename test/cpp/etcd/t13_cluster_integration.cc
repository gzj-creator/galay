/**
 * @file t13_cluster_integration.cc
 * @brief 用途：验证 sync cluster client 的多端点基本读写与跳过逻辑。
 */

#include <galay/cpp/galay-etcd/cluster/etcd_cluster_client.h>

#include "integration_config.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

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

std::string nowSuffix()
{
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

} // namespace

int main()
{
    if (const int skip_code = etcd_test::requireIntegrationEnabledOrSkip("etcd.it.cluster");
        skip_code != 0) {
        return skip_code;
    }

    const auto endpoints = parseEndpoints(std::getenv("GALAY_ETCD_ENDPOINTS"));
    if (endpoints.size() < 2) {
        std::cout << "[SKIP] etcd.it.cluster requires GALAY_ETCD_ENDPOINTS with at least 2 endpoints\n";
        return etcd_test::kEtcdTestSkippedExitCode;
    }

    galay::etcd::EtcdProductionConfig production;
    production.endpoints = endpoints;
    production.endpoint_policy = galay::etcd::EtcdEndpointPolicy::RoundRobin;
    production.retry.attempts = 3;
    production.retry.initial_backoff = std::chrono::milliseconds(5);
    production.retry.max_backoff = std::chrono::milliseconds(20);
    production.retry.jitter = false;

    galay::etcd::EtcdClusterClient client = galay::etcd::EtcdClusterClientBuilder()
        .productionConfig(production)
        .build();

    auto client_lease = client.tryAcquire();
    if (!client_lease.has_value()) {
        return fail("cluster acquire failed: " + client_lease.error().message());
    }
    auto connect = client_lease->get()->connect();
    if (!connect.has_value()) {
        return fail("cluster connect failed: " + connect.error().message());
    }

    const std::string key = "/galay-etcd/cluster/" + nowSuffix();
    const std::string value = "value-" + nowSuffix();

    auto put = client_lease->get()->put(key, value);
    if (!put.has_value()) {
        return fail("cluster put failed: " + put.error().message());
    }

    auto get = client_lease->get()->get(key);
    if (!get.has_value()) {
        return fail("cluster get failed: " + get.error().message());
    }
    if (get->empty() || get->front().value != value) {
        return fail("cluster get value mismatch");
    }

    auto del = client_lease->get()->del(key);
    if (!del.has_value() || *del <= 0) {
        return fail("cluster delete failed");
    }

    auto close = client_lease->get()->close();
    if (!close.has_value()) {
        return fail("cluster close failed: " + close.error().message());
    }

    std::cout << "ETCD CLUSTER INTEGRATION TEST PASSED\n";
    return 0;
}
