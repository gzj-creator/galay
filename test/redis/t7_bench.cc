#include "redis/async/redis_client.h"
#include <kernel/kernel/runtime.h>
#include <iostream>
#include <chrono>
#include <atomic>
#include <array>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

using namespace galay::redis;
using namespace galay::kernel;

// 全局统计
std::atomic<int> success_count{0};
std::atomic<int> error_count{0};
std::atomic<int> timeout_count{0};
std::atomic<int> completed_clients{0};
std::mutex completed_mutex;
std::condition_variable completed_cv;

void markClientCompleted()
{
    completed_clients.fetch_add(1, std::memory_order_relaxed);
    completed_cv.notify_one();
}

/**
 * @brief 单个客户端的性能测试
 */
Coroutine benchmarkClient(IOScheduler* scheduler, int client_id, int operations_per_client, bool verbose)
{
    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder command_builder;

    // 连接到Redis服务器
    auto connect_result = co_await client.connect("127.0.0.1", 6379);
    if (!connect_result) {
        std::cerr << "Client " << client_id << " failed to connect: "
                  << connect_result.error().message() << std::endl;
        error_count.fetch_add(operations_per_client * 2, std::memory_order_relaxed);
        markClientCompleted();
        co_return;
    }

    if (verbose) {
        std::cout << "Client " << client_id << " connected" << std::endl;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    int local_success = 0;
    int local_error = 0;
    int local_timeout = 0;

    // 执行操作
    for (int i = 0; i < operations_per_client; ++i) {
        std::string key = "bench_" + std::to_string(client_id) + "_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i);

        // SET操作
        auto set_result = co_await client.command(command_builder.set(key, value))
                              .timeout(std::chrono::seconds(5));
        if (set_result && set_result.value()) {
            ++local_success;
        } else if (!set_result) {
            if (set_result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
                ++local_timeout;
            } else {
                ++local_error;
            }
        }

        // GET操作
        auto get_result = co_await client.command(command_builder.get(key))
                              .timeout(std::chrono::seconds(5));
        if (get_result && get_result.value()) {
            ++local_success;
        } else if (!get_result) {
            if (get_result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
                ++local_timeout;
            } else {
                ++local_error;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    success_count.fetch_add(local_success, std::memory_order_relaxed);
    error_count.fetch_add(local_error, std::memory_order_relaxed);
    timeout_count.fetch_add(local_timeout, std::memory_order_relaxed);

    if (verbose) {
        std::cout << "Client " << client_id << " completed " << (operations_per_client * 2)
                  << " operations in " << duration.count() << "ms" << std::endl;
    }

    co_await client.close();
    markClientCompleted();
}

/**
 * @brief Pipeline性能测试
 */
Coroutine benchmarkPipeline(IOScheduler* scheduler, int client_id, int batch_size, int batches, bool verbose)
{
    auto client = RedisClientBuilder().scheduler(scheduler).build();

    auto connect_result = co_await client.connect("127.0.0.1", 6379);
    if (!connect_result) {
        std::cerr << "Pipeline client " << client_id << " failed to connect" << std::endl;
        error_count.fetch_add(batch_size * batches, std::memory_order_relaxed);
        markClientCompleted();
        co_return;
    }

    if (verbose) {
        std::cout << "Pipeline client " << client_id << " connected" << std::endl;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    int local_success = 0;
    int local_error = 0;
    int local_timeout = 0;

    for (int batch = 0; batch < batches; ++batch) {
        RedisCommandBuilder builder;
        builder.reserve(static_cast<size_t>(batch_size), static_cast<size_t>(batch_size) * 2U, 96U * static_cast<size_t>(batch_size));

        // 构建批量命令
        for (int i = 0; i < batch_size; ++i) {
            std::string key = "pipeline_" + std::to_string(client_id) + "_" + std::to_string(batch * batch_size + i);
            std::string value = "value_" + std::to_string(i);
            builder.append("SET", std::array<std::string_view, 2>{key, value});
        }

        // 执行Pipeline
        auto result = co_await client.batch(builder.commands());
        if (result && result.value()) {
            local_success += batch_size;
        } else if (!result) {
            if (result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
                local_timeout += batch_size;
            } else {
                local_error += batch_size;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    success_count.fetch_add(local_success, std::memory_order_relaxed);
    error_count.fetch_add(local_error, std::memory_order_relaxed);
    timeout_count.fetch_add(local_timeout, std::memory_order_relaxed);

    if (verbose) {
        std::cout << "Pipeline client " << client_id << " completed " << (batch_size * batches)
                  << " operations in " << duration.count() << "ms" << std::endl;
    }

    co_await client.close();
    markClientCompleted();
}

int main(int argc, char* argv[])
{
    // 默认参数
    int num_clients = 10;
    int operations_per_client = 100;
    bool use_pipeline = false;
    int batch_size = 10;
    bool verbose = true;

    // 解析命令行参数
    if (argc > 1) num_clients = std::atoi(argv[1]);
    if (argc > 2) operations_per_client = std::atoi(argv[2]);
    if (argc > 3) use_pipeline = (std::string(argv[3]) == "pipeline");
    if (argc > 4) batch_size = std::atoi(argv[4]);
    if (argc > 5) verbose = (std::string(argv[5]) != "quiet");
    if (num_clients >= 50 && argc <= 5) {
        verbose = false;
    }

    std::cout << "==================================================" << std::endl;
    std::cout << "Redis Client Performance Benchmark" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Clients: " << num_clients << std::endl;
    std::cout << "Operations per client: " << operations_per_client << std::endl;
    std::cout << "Mode: " << (use_pipeline ? "Pipeline" : "Normal") << std::endl;
    if (use_pipeline) {
        std::cout << "Batch size: " << batch_size << std::endl;
    }
    int pipeline_batches = use_pipeline ? (operations_per_client / batch_size) : 0;
    int pipeline_remainder = use_pipeline ? (operations_per_client % batch_size) : 0;
    if (use_pipeline && pipeline_remainder != 0) {
        std::cout << "Warning: operations_per_client is not divisible by batch_size, "
                  << pipeline_remainder << " ops/client are ignored in this run" << std::endl;
    }
    int effective_ops_per_client = use_pipeline ? (pipeline_batches * batch_size) : (operations_per_client * 2);
    std::cout << "Total operations: " << (num_clients * effective_ops_per_client) << std::endl;
    std::cout << "==================================================" << std::endl;

    try {
        Runtime runtime;
        runtime.start();

        success_count.store(0);
        error_count.store(0);
        timeout_count.store(0);
        completed_clients.store(0);

        auto start_time = std::chrono::high_resolution_clock::now();

        // 启动所有客户端
        for (int i = 0; i < num_clients; ++i) {
            auto* scheduler = runtime.getNextIOScheduler();
            if (!scheduler) {
                std::cerr << "Failed to get IO scheduler for client " << i << std::endl;
                runtime.stop();
                return 1;
            }
            if (use_pipeline) {
                scheduleTask(scheduler,
                             benchmarkPipeline(scheduler, i, batch_size, pipeline_batches, verbose));
            } else {
                scheduleTask(scheduler,
                             benchmarkClient(scheduler, i, operations_per_client, verbose));
            }
        }

        // 等待所有客户端完成，而非固定 sleep 30s
        constexpr auto kMaxWait = std::chrono::seconds(120);
        std::unique_lock<std::mutex> lock(completed_mutex);
        const bool finished = completed_cv.wait_for(lock, kMaxWait, [&]() {
            return completed_clients.load(std::memory_order_relaxed) >= num_clients;
        });
        if (!finished) {
            std::cerr << "Benchmark wait timeout after 120s, completed clients: "
                      << completed_clients.load(std::memory_order_relaxed)
                      << "/" << num_clients << std::endl;
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        runtime.stop();

        // 输出统计结果
        std::cout << "\n==================================================" << std::endl;
        std::cout << "Benchmark Results" << std::endl;
        std::cout << "==================================================" << std::endl;
        std::cout << "Total time: " << duration.count() << "ms" << std::endl;
        std::cout << "Successful operations: " << success_count.load() << std::endl;
        std::cout << "Failed operations: " << error_count.load() << std::endl;
        std::cout << "Timeout operations: " << timeout_count.load() << std::endl;

        int total_ops = success_count.load() + error_count.load() + timeout_count.load();
        if (total_ops > 0) {
            double ops_per_sec = (double)success_count.load() / (duration.count() / 1000.0);
            double success_rate = (double)success_count.load() / total_ops * 100.0;

            std::cout << "Operations per second: " << (int)ops_per_sec << std::endl;
            std::cout << "Success rate: " << success_rate << "%" << std::endl;
        }
        std::cout << "==================================================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Benchmark failed with exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
