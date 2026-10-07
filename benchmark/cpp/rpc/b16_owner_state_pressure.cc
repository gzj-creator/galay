#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-rpc/kernel/rpc_call.h>
#include <galay/cpp/galay-rpc/kernel/rpc_endpoint_cache.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>

using namespace galay::rpc;

int main()
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    constexpr size_t iterations = 100000;
    uint64_t observed = 0;
    RpcEndpointCache cache;
    RpcEndpointInfo endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = 7001;
    endpoint.service = "Echo";
    for (size_t i = 0; i < 64; ++i) {
        endpoint.instance_id = std::to_string(i);
        cache.apply(RpcEndpointEvent::add(endpoint));
    }
    auto update = RpcEndpointEvent::update(endpoint);
    std::array<double, 7> cancel_samples{};
    std::array<double, 7> cache_samples{};
    for (size_t sample = 0; sample < 8; ++sample) {
        auto start = RpcClock::now();
        for (size_t i = 0; i < iterations; ++i) {
            RpcCancellationSource source;
            auto registration = source.token().register_callback([&observed] { ++observed; });
            source.cancel();
        }
        auto stop = RpcClock::now();
        const double cancel_ns = std::chrono::duration<double, std::nano>(stop - start).count();
        start = RpcClock::now();
        for (size_t i = 0; i < iterations; ++i) {
            update.endpoint.weight = static_cast<uint32_t>(i % 100 + 1);
            cache.apply(update);
            auto endpoints = cache.selectable("Echo");
            observed += endpoints.back().weight;
        }
        stop = RpcClock::now();
        if (sample != 0) {
            cancel_samples[sample - 1] = cancel_ns / iterations;
            cache_samples[sample - 1] =
                std::chrono::duration<double, std::nano>(stop - start).count() / iterations;
        }
    }
    std::ranges::sort(cancel_samples);
    std::ranges::sort(cache_samples);
    std::cout << "cancel_register_notify_ns_median=" << cancel_samples[3]
              << "\ncache_update_select_64_ns_median=" << cache_samples[3]
              << "\nobserved=" << observed << '\n';
    return observed == 8 * iterations * 103 / 2 ? 0 : 1;
}
