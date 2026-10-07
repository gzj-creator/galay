import galay.etcd;

#include <galay/cpp/galay-kernel/core/runtime.h>

#include <iostream>

int main()
{
    galay::etcd::EtcdProductionConfig production;
    production.endpoints = {
        "http://127.0.0.1:2379",
        "http://127.0.0.1:22379",
    };
    production.connections_per_endpoint = 2;

    auto sync_pool = galay::etcd::EtcdClusterClientBuilder()
        .production_config(production)
        .build();
    auto sync_lease = sync_pool.try_acquire();
    if (!sync_lease.has_value()) {
        std::cerr << "sync acquire failed: " << sync_lease.error().message() << '\n';
        return 1;
    }
    std::cout << "sync pool size=" << sync_pool.size()
              << " idle=" << sync_pool.idle_count() << '\n';
    sync_lease->release();

    auto runtime = galay::kernel::RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();
    auto start_result = runtime.start();
    if (!start_result.has_value()) {
        std::cerr << "runtime start failed\n";
        return 2;
    }
    auto* scheduler = runtime.get_next_io_scheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "runtime has no IO scheduler\n";
        return 3;
    }

    auto async_pool = galay::etcd::AsyncEtcdClusterClientBuilder()
        .scheduler(scheduler)
        .production_config(production)
        .build();
    auto async_lease = async_pool.try_acquire();
    if (!async_lease.has_value()) {
        runtime.stop();
        std::cerr << "async acquire failed: " << async_lease.error().message() << '\n';
        return 4;
    }
    std::cout << "async pool size=" << async_pool.size()
              << " idle=" << async_pool.idle_count() << '\n';
    async_lease->release();
    runtime.stop();
    return 0;
}
