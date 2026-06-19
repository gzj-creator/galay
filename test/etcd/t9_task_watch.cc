#include "galay-etcd/async/client.h"

#include <galay-kernel/concurrency/async_waiter.h>
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

Task<void> runTaskWatch(IOScheduler* scheduler,
                        std::string endpoint,
                        std::atomic<bool>* done,
                        int* exit_code)
{
    struct TaskWatchState {
        std::atomic<bool> seen{false};
        std::atomic<bool> matched{false};
        galay::kernel::AsyncWaiter<void> observed;
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

    const std::string key = "/galay-etcd/async-task-watch/" + nowSuffix();
    const std::string value = "watch-value-" + nowSuffix();
    auto watch_state = std::make_shared<TaskWatchState>();

    auto watch_started = client.watch(
        key,
        AsyncEtcdClient::WatchTaskHandler(
            [watch_state, value](galay::etcd::EtcdWatchResponse response) -> Task<void> {
                if (!response.events.empty()) {
                    watch_state->matched.store(
                        response.events.front().kv.value == value,
                        std::memory_order_release);
                    watch_state->seen.store(true, std::memory_order_release);
                    watch_state->observed.notify();
                }
                co_return;
            }));
    if (!watch_started.has_value()) {
        finish(fail("task watch start failed: " + watch_started.error().message()));
        co_return;
    }

    auto put = co_await client.put(key, value);
    if (!put.has_value()) {
        finish(fail("put failed: " + put.error().message()));
        co_return;
    }

    auto observed = co_await watch_state->observed.wait().timeout(std::chrono::seconds(3));
    if (!observed || !watch_state->seen.load(std::memory_order_acquire)) {
        finish(fail("task watch did not observe put"));
        co_return;
    }
    if (!watch_state->matched.load(std::memory_order_acquire)) {
        finish(fail("task watch value mismatch"));
        co_return;
    }

    auto close = co_await client.close();
    if (!close.has_value()) {
        finish(fail("close failed: " + close.error().message()));
        co_return;
    }

    std::cout << "ASYNC TASK WATCH TEST PASSED\n";
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
    if (!galay::kernel::scheduleTask(scheduler, runTaskWatch(scheduler, endpoint, &done, &exit_code))) {
        runtime.stop();
        return fail("failed to schedule async task watch test");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!done.load(std::memory_order_acquire)) {
        exit_code = fail("async task watch test timeout");
    }

    runtime.stop();
    return exit_code;
}
