/**
 * @file b3_discovery.cpp
 * @brief ServiceDiscovery 压测
 *
 * @details 专门测试 AsyncLocalServiceRegistry 的性能
 *
 * 注意：每个 worker 使用独立的 registry 实例，避免共享状态竞争。
 */

#include "rpc/kernel/rpc_discovery.h"
#include "rpc/utils/runtime_compat.h"
#include "kernel/kernel/runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <csignal>

using namespace galay::rpc;
using namespace galay::kernel;

std::atomic<uint64_t> g_register_ops{0};
std::atomic<uint64_t> g_discover_ops{0};
std::atomic<uint64_t> g_deregister_ops{0};
std::atomic<uint64_t> g_errors{0};
std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running.store(false);
}

/**
 * @brief 压测协程 - 每个 worker 使用独立的 registry
 */
Coroutine benchWorker(size_t worker_id) {
    // 每个 worker 独立的 registry，避免共享状态竞争
    AsyncLocalServiceRegistry registry;

    ServiceEndpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = 9000 + (worker_id % 100);
    endpoint.service_name = "BenchService" + std::to_string(worker_id % 10);
    endpoint.instance_id = "instance-" + std::to_string(worker_id);
    endpoint.weight = 100;

    while (g_running.load(std::memory_order_relaxed)) {
        // 注册服务
        co_await registry.registerServiceAsync(endpoint);
        if (!registry.lastError().isOk()) {
            g_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        g_register_ops.fetch_add(1, std::memory_order_relaxed);

        // 发现服务
        co_await registry.discoverServiceAsync(endpoint.service_name);
        if (!registry.lastError().isOk()) {
            g_errors.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_discover_ops.fetch_add(1, std::memory_order_relaxed);
        }

        // 注销服务
        co_await registry.deregisterServiceAsync(endpoint);
        if (!registry.lastError().isOk()) {
            g_errors.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_deregister_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    co_return;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    size_t num_workers = 100;
    size_t duration_sec = 10;
    size_t io_schedulers = 2;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) break;
        std::string opt = argv[i];
        std::string val = argv[i + 1];

        if (opt == "-w") num_workers = std::stoul(val);
        else if (opt == "-d") duration_sec = std::stoul(val);
        else if (opt == "-i") io_schedulers = std::stoul(val);
        else if (opt == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -w <workers>     Number of workers (default: 100)\n"
                      << "  -d <duration>    Test duration in seconds (default: 10)\n"
                      << "  -i <io_count>    IO scheduler count (default: 2)\n";
            return 0;
        }
    }

    std::cout << "=== ServiceDiscovery Benchmark ===\n";
    std::cout << "Workers: " << num_workers << "\n";
    std::cout << "Duration: " << duration_sec << " seconds\n";
    const size_t resolved_io_schedulers = resolveIoSchedulerCount(io_schedulers);
    std::cout << "IO Schedulers: "
              << (io_schedulers == 0
                      ? "auto (" + std::to_string(resolved_io_schedulers) + ")"
                      : std::to_string(resolved_io_schedulers))
              << "\n\n";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(resolved_io_schedulers).computeSchedulerCount(1).build();
    runtime.start();

    std::cout << "Starting " << num_workers << " workers...\n";
    bool schedule_failed = false;
    for (size_t i = 0; i < num_workers; ++i) {
        auto* scheduler = runtime.getNextIOScheduler();
        if (scheduler == nullptr || !scheduleTask(scheduler, benchWorker(i))) {
            schedule_failed = true;
            break;
        }
    }
    if (schedule_failed) {
        std::cerr << "Failed to schedule service discovery benchmark workers\n";
        g_running.store(false, std::memory_order_relaxed);
        runtime.stop();
        return 1;
    }

    std::cout << "Benchmark running...\n\n";

    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_register = 0;
    uint64_t last_discover = 0;
    uint64_t last_deregister = 0;

    for (size_t sec = 0; sec < duration_sec && g_running.load(); ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t cur_register = g_register_ops.load();
        uint64_t cur_discover = g_discover_ops.load();
        uint64_t cur_deregister = g_deregister_ops.load();
        uint64_t cur_errors = g_errors.load();

        uint64_t reg_ops = cur_register - last_register;
        uint64_t disc_ops = cur_discover - last_discover;
        uint64_t dereg_ops = cur_deregister - last_deregister;
        uint64_t total_ops = reg_ops + disc_ops + dereg_ops;

        std::cout << "[" << std::setw(3) << (sec + 1) << "s] "
                  << "Total: " << std::setw(8) << total_ops << " ops/s"
                  << " | Reg: " << std::setw(7) << reg_ops
                  << " | Disc: " << std::setw(7) << disc_ops
                  << " | Dereg: " << std::setw(7) << dereg_ops
                  << " | Errors: " << cur_errors
                  << "\n";

        last_register = cur_register;
        last_discover = cur_discover;
        last_deregister = cur_deregister;
    }

    g_running.store(false);
    auto end_time = std::chrono::steady_clock::now();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    runtime.stop();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    double duration_s = duration.count() / 1000.0;

    uint64_t total_register = g_register_ops.load();
    uint64_t total_discover = g_discover_ops.load();
    uint64_t total_deregister = g_deregister_ops.load();
    uint64_t total_errors = g_errors.load();
    uint64_t total_ops = total_register + total_discover + total_deregister;

    std::cout << "\n=== Benchmark Results ===\n";
    std::cout << "Duration: " << std::fixed << std::setprecision(2) << duration_s << " seconds\n";
    std::cout << "Total Operations: " << total_ops << "\n";
    std::cout << "  - Register: " << total_register << "\n";
    std::cout << "  - Discover: " << total_discover << "\n";
    std::cout << "  - Deregister: " << total_deregister << "\n";
    std::cout << "Total Errors: " << total_errors << "\n";
    std::cout << "Average OPS: " << std::fixed << std::setprecision(0) << (total_ops / duration_s) << "\n";
    std::cout << "Error Rate: " << std::fixed << std::setprecision(2)
              << (total_ops > 0 ? (100.0 * total_errors / (total_ops + total_errors)) : 0) << "%\n";

    return 0;
}
