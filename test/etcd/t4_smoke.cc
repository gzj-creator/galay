#include "galay-etcd/async/client.h"

#include <galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using galay::etcd::AsyncEtcdClient;
using galay::etcd::EtcdConfig;
using galay::kernel::IOScheduler;
using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::kernel::Task;

namespace
{

std::string nowSuffix()
{
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

int fail(const std::string& message)
{
    std::cerr << "[FAIL] " << message << '\n';
    return 1;
}

Task<void> runSmoke(IOScheduler* scheduler,
                    std::string endpoint,
                    std::atomic<bool>* done,
                    int* exit_code)
{
    struct ThreadWatchState {
        std::atomic<bool> seen{false};
    };

    auto finish = [&](int code) {
        *exit_code = code;
        done->store(true, std::memory_order_release);
    };

    EtcdConfig config;
    config.endpoint = endpoint;
    config.api_prefix = "/v3";

    auto client = galay::etcd::AsyncEtcdClientBuilder().scheduler(scheduler).config(config).build();

    auto conn = co_await client.connect();
    if (!conn.has_value()) {
        finish(fail("connect failed: " + conn.error().message()));
        co_return;
    }
    std::cout << "[OK] connect\n";

    const std::string key = "/galay-etcd/async-smoke/" + nowSuffix();
    const std::string value = "v-" + nowSuffix();
    auto watch_state = std::make_shared<ThreadWatchState>();

    auto watch_started = client.watch(
        key,
        [watch_state](galay::etcd::EtcdWatchResponse response) {
            if (response.events.empty()) {
                return;
            }
            watch_state->seen.store(true, std::memory_order_release);
        });
    if (!watch_started.has_value()) {
        finish(fail("watch start failed: " + watch_started.error().message()));
        co_return;
    }

    auto put = co_await client.put(key, value);
    if (!put.has_value()) {
        finish(fail("put failed: " + put.error().message()));
        co_return;
    }

    const auto watch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!watch_state->seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < watch_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!watch_state->seen.load(std::memory_order_acquire)) {
        finish(fail("watch did not observe put"));
        co_return;
    }

    auto get = co_await client.get(key);
    if (!get.has_value()) {
        finish(fail("get failed: " + get.error().message()));
        co_return;
    }
    if (get.value().empty() || get.value().front().value != value) {
        finish(fail("get value mismatch"));
        co_return;
    }

    auto del = co_await client.del(key);
    if (!del.has_value()) {
        finish(fail("delete failed: " + del.error().message()));
        co_return;
    }
    if (del.value() <= 0) {
        finish(fail("delete count should be > 0"));
        co_return;
    }

    auto lease = co_await client.grantLease(3);
    if (!lease.has_value()) {
        finish(fail("grant lease failed: " + lease.error().message()));
        co_return;
    }
    if (lease.value() <= 0) {
        finish(fail("lease id should be > 0"));
        co_return;
    }

    const std::string lease_key = key + "/lease";
    auto put_lease = co_await client.put(lease_key, value, lease.value());
    if (!put_lease.has_value()) {
        finish(fail("put with lease failed: " + put_lease.error().message()));
        co_return;
    }

    auto get_lease = co_await client.get(lease_key);
    if (!get_lease.has_value()) {
        finish(fail("get leased key failed: " + get_lease.error().message()));
        co_return;
    }
    if (get_lease.value().empty()) {
        finish(fail("leased key should exist immediately"));
        co_return;
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));
    auto get_after_ttl = co_await client.get(lease_key);
    if (!get_after_ttl.has_value()) {
        finish(fail("get after ttl failed: " + get_after_ttl.error().message()));
        co_return;
    }
    if (!get_after_ttl.value().empty()) {
        finish(fail("leased key should expire after ttl"));
        co_return;
    }

    auto close = co_await client.close();
    if (!close.has_value()) {
        finish(fail("close failed: " + close.error().message()));
        co_return;
    }

    std::cout << "ASYNC SMOKE TEST PASSED\n";
    finish(0);
}

} // namespace

int main(int argc, char** argv)
{
    const std::string endpoint = argc > 1 ? argv[1] : "http://127.0.0.1:2379";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        return fail("failed to get io scheduler");
    }

    std::atomic<bool> done{false};
    int exit_code = 1;
    if (!galay::kernel::scheduleTask(scheduler, runSmoke(scheduler, endpoint, &done, &exit_code))) {
        runtime.stop();
        return fail("failed to schedule async smoke task");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!done.load(std::memory_order_acquire)) {
        exit_code = fail("async smoke test timeout");
    }

    runtime.stop();
    return exit_code;
}
