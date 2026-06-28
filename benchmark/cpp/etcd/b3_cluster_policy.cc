#include <galay/cpp/galay-etcd/cluster/etcd_cluster_client.h>

#include <chrono>
#include <cstdint>
#include <iostream>

int main()
{
    galay::etcd::EtcdProductionConfig production;
    production.endpoints = {
        "http://127.0.0.1:2379",
        "http://127.0.0.1:22379",
        "http://127.0.0.1:32379",
    };
    production.endpoint_policy = galay::etcd::EtcdEndpointPolicy::RoundRobin;
    production.retry.initial_backoff = std::chrono::milliseconds(5);
    production.retry.max_backoff = std::chrono::milliseconds(40);
    production.retry.jitter = false;

    galay::etcd::EtcdClusterState state(production);
    constexpr int64_t iterations = 100000;

    const auto begin = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iterations; ++i) {
        auto index = state.selectEndpoint();
        if (!index.has_value()) {
            std::cerr << "selectEndpoint failed\n";
            return 1;
        }
        if ((i % 7) == 0) {
            state.markFailure(*index, galay::etcd::EtcdError(galay::etcd::EtcdErrorType::Connection, "bench"), true);
        } else {
            state.markSuccess(*index);
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();

    const auto stats = state.getStats();
    std::cout << "Iterations : " << iterations << '\n';
    std::cout << "Elapsed us : " << elapsed_us << '\n';
    std::cout << "Failures   : " << stats.request_failures << '\n';
    std::cout << "Switches   : " << stats.endpoint_switches << '\n';
    return 0;
}
