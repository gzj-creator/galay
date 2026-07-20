/**
 * @file t14_async_cluster_integration.cc
 * @brief 验证 AsyncEtcdClusterClient 租约池可取得 client 并完成真实 KV 请求。
 */

#include <galay/cpp/galay-etcd/async/client.h>

#include "integration_config.h"

#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
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
    return std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

galay::kernel::Task<void> runPoolCase(
    galay::kernel::IOScheduler* scheduler,
    galay::etcd::EtcdProductionConfig production,
    std::atomic<bool>* done,
    int* exit_code)
{
    auto finish = [&](int code) {
        *exit_code = code;
        done->store(true, std::memory_order_release);
    };

    const size_t expected_size =
        production.endpoints.size() * production.connections_per_endpoint;
    auto pool = galay::etcd::AsyncEtcdClusterClientBuilder()
        .scheduler(scheduler)
        .productionConfig(std::move(production))
        .build();
    if (pool.size() != expected_size || pool.idleCount() != expected_size) {
        finish(fail("async cluster pool size mismatch"));
        co_return;
    }

    auto lease = pool.tryAcquire();
    if (!lease.has_value()) {
        finish(fail("async cluster acquire failed: " + lease.error().message()));
        co_return;
    }

    auto connect = co_await lease->get()->connect();
    if (!connect.has_value()) {
        finish(fail("async cluster connect failed: " + connect.error().message()));
        co_return;
    }

    const std::string key = "/galay-etcd/async-cluster-pool/" + nowSuffix();
    const std::string value = "value-" + nowSuffix();
    auto put = co_await lease->get()->put(key, value);
    if (!put.has_value()) {
        finish(fail("async cluster put failed: " + put.error().message()));
        co_return;
    }

    auto get = co_await lease->get()->get(key);
    if (!get.has_value()) {
        finish(fail("async cluster get failed: " + get.error().message()));
        co_return;
    }
    if (get->empty() || get->front().value != value) {
        finish(fail("async cluster get value mismatch"));
        co_return;
    }

    auto del = co_await lease->get()->del(key);
    if (!del.has_value() || *del <= 0) {
        finish(fail("async cluster delete failed"));
        co_return;
    }

    auto close = co_await lease->get()->close();
    if (!close.has_value()) {
        finish(fail("async cluster close failed: " + close.error().message()));
        co_return;
    }

    lease->release();
    if (pool.idleCount() != pool.size()) {
        finish(fail("async cluster lease should return client to pool"));
        co_return;
    }

    std::cout << "ETCD ASYNC CLUSTER POOL INTEGRATION TEST PASSED\n";
    finish(0);
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

    galay::etcd::EtcdProductionConfig production;
    production.endpoints = endpoints;
    production.connections_per_endpoint = 2;

    galay::kernel::Runtime runtime = galay::kernel::RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    auto start_result = runtime.start();
    if (!start_result.has_value()) {
        return fail("runtime start failed");
    }

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        return fail("failed to get io scheduler");
    }

    std::atomic<bool> done{false};
    int exit_code = 1;
    const bool scheduled = galay::kernel::scheduleTask(
        scheduler,
        runPoolCase(scheduler, std::move(production), &done, &exit_code));
    if (!scheduled) {
        runtime.stop();
        return fail("failed to schedule async cluster pool task");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done.load(std::memory_order_acquire)) {
        exit_code = fail("async cluster pool integration timeout");
    }

    runtime.stop();
    return exit_code;
}
