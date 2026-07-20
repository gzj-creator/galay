/**
 * @file t18_cluster_connection_reuse.cc
 * @brief 验证同步与异步 cluster client 的无锁租约池语义。
 */

#include <galay/cpp/galay-etcd/async/client.h>
#include <galay/cpp/galay-etcd/cluster/etcd_cluster_client.h>

#include <atomic>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

int fail(const std::string& message)
{
    std::cerr << "[FAIL] " << message << '\n';
    return 1;
}

galay::etcd::EtcdProductionConfig makeProduction(size_t connections_per_endpoint)
{
    galay::etcd::EtcdProductionConfig production;
    production.endpoints = {
        "http://127.0.0.1:2379",
        "http://127.0.0.1:22379",
    };
    production.connections_per_endpoint = connections_per_endpoint;
    return production;
}

template <typename Pool>
bool stressPool(Pool& pool)
{
    constexpr size_t kThreads = 8;
    constexpr size_t kIterations = 5000;
    constexpr size_t kAcquireRetries = 100000;

    std::atomic<bool> failed{false};
    std::mutex active_mutex;
    std::unordered_set<const void*> active_clients;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&pool, &failed, &active_mutex, &active_clients] {
            for (size_t iteration = 0; iteration < kIterations; ++iteration) {
                bool acquired = false;
                for (size_t retry = 0; retry < kAcquireRetries; ++retry) {
                    auto lease = pool.tryAcquire();
                    if (lease.has_value()) {
                        const void* const client = lease->get();
                        if (client == nullptr) {
                            failed.store(true, std::memory_order_release);
                            break;
                        }
                        {
                            std::lock_guard lock(active_mutex);
                            const auto inserted = active_clients.insert(client);
                            if (!inserted.second) {
                                failed.store(true, std::memory_order_release);
                            }
                        }
                        std::this_thread::yield();
                        {
                            std::lock_guard lock(active_mutex);
                            if (active_clients.erase(client) != 1) {
                                failed.store(true, std::memory_order_release);
                            }
                        }
                        acquired = true;
                        break;
                    }
                    if (lease.error().type() != galay::etcd::EtcdErrorType::PoolExhausted) {
                        failed.store(true, std::memory_order_release);
                        break;
                    }
                    std::this_thread::yield();
                }
                if (!acquired) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }
    return !failed.load(std::memory_order_acquire);
}

} // namespace

int main()
{
    using galay::etcd::AsyncEtcdClientLease;
    using galay::etcd::EtcdClientLease;

    static_assert(!std::is_copy_constructible_v<EtcdClientLease>);
    static_assert(!std::is_copy_assignable_v<EtcdClientLease>);
    static_assert(std::is_nothrow_move_constructible_v<EtcdClientLease>);
    static_assert(std::is_nothrow_move_assignable_v<EtcdClientLease>);
    static_assert(!std::is_copy_constructible_v<AsyncEtcdClientLease>);
    static_assert(!std::is_copy_assignable_v<AsyncEtcdClientLease>);
    static_assert(std::is_nothrow_move_constructible_v<AsyncEtcdClientLease>);
    static_assert(std::is_nothrow_move_assignable_v<AsyncEtcdClientLease>);

    auto sync_pool = galay::etcd::EtcdClusterClientBuilder()
        .productionConfig(makeProduction(1))
        .connectionsPerEndpoint(2)
        .build();
    if (sync_pool.size() != 4 || sync_pool.idleCount() != 4) {
        return fail("sync pool should create two clients for each endpoint");
    }

    std::vector<EtcdClientLease> sync_leases;
    sync_leases.reserve(sync_pool.size());
    for (size_t index = 0; index < sync_pool.size(); ++index) {
        auto lease = sync_pool.tryAcquire();
        if (!lease.has_value() || lease->get() == nullptr) {
            return fail("sync pool should lease every configured client");
        }
        sync_leases.push_back(std::move(*lease));
    }
    auto exhausted_sync = sync_pool.tryAcquire();
    if (exhausted_sync.has_value() ||
        exhausted_sync.error().type() != galay::etcd::EtcdErrorType::PoolExhausted) {
        return fail("sync pool exhaustion should return PoolExhausted");
    }

    sync_leases.back().release();
    sync_leases.pop_back();
    if (sync_pool.idleCount() != 1) {
        return fail("sync lease release should return the client");
    }
    auto reacquired_sync = sync_pool.tryAcquire();
    if (!reacquired_sync.has_value()) {
        return fail("sync pool should reacquire a returned client");
    }
    reacquired_sync->release();
    sync_leases.clear();
    if (sync_pool.idleCount() != sync_pool.size()) {
        return fail("sync lease destruction should return all clients");
    }
    if (!stressPool(sync_pool)) {
        return fail("sync pool concurrent acquire/release stress failed");
    }
    if (sync_pool.idleCount() != sync_pool.size()) {
        return fail("sync pool idle count should recover after concurrent stress");
    }

    auto async_pool = galay::etcd::AsyncEtcdClusterClientBuilder()
        .scheduler(nullptr)
        .productionConfig(makeProduction(1))
        .connectionsPerEndpoint(2)
        .build();
    if (async_pool.size() != 4 || async_pool.idleCount() != 4) {
        return fail("async pool should create two clients for each endpoint");
    }

    auto async_lease = async_pool.tryAcquire();
    if (!async_lease.has_value() || async_lease->get() == nullptr) {
        return fail("async pool should return an AsyncEtcdClient lease");
    }
    galay::etcd::AsyncEtcdClusterClient moved_async_pool(std::move(async_pool));
    async_lease->release();
    if (moved_async_pool.idleCount() != moved_async_pool.size()) {
        return fail("lease acquired before pool move should return to moved pool state");
    }
    if (!stressPool(moved_async_pool)) {
        return fail("async pool concurrent acquire/release stress failed");
    }
    if (moved_async_pool.idleCount() != moved_async_pool.size()) {
        return fail("async pool idle count should recover after concurrent stress");
    }

    auto empty_sync_pool = galay::etcd::EtcdClusterClientBuilder()
        .productionConfig(makeProduction(1))
        .connectionsPerEndpoint(0)
        .build();
    auto invalid_sync = empty_sync_pool.tryAcquire();
    if (invalid_sync.has_value() ||
        invalid_sync.error().type() != galay::etcd::EtcdErrorType::InvalidParam) {
        return fail("zero connections per endpoint should be rejected");
    }

    auto empty_async_pool = galay::etcd::AsyncEtcdClusterClientBuilder()
        .productionConfig(makeProduction(1))
        .connectionsPerEndpoint(0)
        .build();
    auto invalid_async = empty_async_pool.tryAcquire();
    if (invalid_async.has_value() ||
        invalid_async.error().type() != galay::etcd::EtcdErrorType::InvalidParam) {
        return fail("async zero connections per endpoint should be rejected");
    }

    galay::etcd::EtcdConfig empty_config;
    empty_config.endpoint.clear();
    empty_config.production.endpoints.clear();
    galay::etcd::EtcdClusterClient no_endpoint_sync(empty_config);
    auto no_endpoint_sync_result = no_endpoint_sync.tryAcquire();
    if (no_endpoint_sync_result.has_value() ||
        no_endpoint_sync_result.error().type() != galay::etcd::EtcdErrorType::InvalidEndpoint) {
        return fail("sync pool without endpoints should return InvalidEndpoint");
    }
    galay::etcd::AsyncEtcdClusterClient no_endpoint_async(nullptr, std::move(empty_config));
    auto no_endpoint_async_result = no_endpoint_async.tryAcquire();
    if (no_endpoint_async_result.has_value() ||
        no_endpoint_async_result.error().type() != galay::etcd::EtcdErrorType::InvalidEndpoint) {
        return fail("async pool without endpoints should return InvalidEndpoint");
    }

    std::cout << "ETCD CLUSTER CLIENT POOL TEST PASSED\n";
    return 0;
}
