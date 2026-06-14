#include "etcd/async/client.h"

#include <kernel/kernel/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using galay::kernel::IOScheduler;
using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::kernel::Task;

namespace
{

Task<void> runExample(IOScheduler* scheduler,
                      std::string endpoint,
                      std::atomic<bool>* done,
                      int* exit_code)
{
    auto finish = [&](int code) {
        *exit_code = code;
        done->store(true, std::memory_order_release);
    };

    const std::string key = "/etcd/examples/async/" + std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count());
    const std::string value = "hello-async";

    galay::etcd::EtcdConfig cfg;
    cfg.endpoint = endpoint;

    auto client = galay::etcd::AsyncEtcdClientBuilder().scheduler(scheduler).config(cfg).build();
    auto conn = co_await client.connect();
    if (!conn.has_value()) {
        std::cerr << "connect failed: " << conn.error().message() << '\n';
        finish(1);
        co_return;
    }

    auto put = co_await client.put(key, value);
    if (!put.has_value()) {
        std::cerr << "put failed: " << put.error().message() << '\n';
        finish(2);
        co_return;
    }

    auto get = co_await client.get(key);
    if (!get.has_value()) {
        std::cerr << "get failed: " << get.error().message() << '\n';
        finish(3);
        co_return;
    }
    if (get.value().empty()) {
        std::cerr << "get returned empty kvs\n";
        finish(4);
        co_return;
    }

    std::cout << "async example ok: " << get.value().front().key
              << " => " << get.value().front().value << '\n';

    (void)co_await client.del(key);
    (void)co_await client.close();

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
        std::cerr << "failed to get io scheduler\n";
        return 1;
    }

    std::atomic<bool> done{false};
    int exit_code = 1;
    if (!galay::kernel::scheduleTask(scheduler, runExample(scheduler, endpoint, &done, &exit_code))) {
        runtime.stop();
        std::cerr << "failed to schedule async example task\n";
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done.load(std::memory_order_acquire)) {
        exit_code = 2;
        std::cerr << "async example timeout\n";
    }

    runtime.stop();
    return exit_code;
}
