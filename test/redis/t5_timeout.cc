#include "galay-redis/async/redis_client.h"
#include <galay-kernel/core/runtime.h>
#include <iostream>
#include <chrono>
#include <array>

using namespace galay::redis;
using namespace galay::kernel;

/**
 * @brief 测试RedisClient的超时功能
 * @details 演示如何使用timeout()方法为Redis命令设置超时
 */
Task<void> test_redis_client_with_timeout(IOScheduler* scheduler)
{
    // 创建RedisClient
    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder command_builder;

    // 连接到Redis服务器
    auto connect_result = co_await client.connect("127.0.0.1", 6379).timeout(std::chrono::seconds(5));
    if (!connect_result) {
        std::cerr << "Failed to connect: " << connect_result.error().message() << std::endl;
        co_return;
    }

    std::cout << "Connected to Redis server" << std::endl;

    // ==================== 测试1: 正常命令（无超时） ====================
    std::cout << "\n=== Test 1: Normal command without timeout ===" << std::endl;

    auto set_result = co_await client.command(command_builder.set("test_key", "test_value"));
    if (set_result && set_result.value()) {
        std::cout << "SET command succeeded" << std::endl;
    } else if (!set_result) {
        std::cerr << "SET command failed: " << set_result.error().message() << std::endl;
    }

    // ==================== 测试2: 带超时的命令 ====================
    std::cout << "\n=== Test 2: Command with 5 second timeout ===" << std::endl;

    // 使用timeout()方法设置5秒超时
    auto get_result = co_await client.command(command_builder.get("test_key"))
                          .timeout(std::chrono::seconds(5));
    if (get_result && get_result.value()) {
        auto& values = get_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << "GET command succeeded: " << values[0].toString() << std::endl;
        }
    } else if (!get_result) {
        std::cerr << "GET command failed: " << get_result.error().message() << std::endl;
    }

    // ==================== 测试3: 短超时（可能触发超时） ====================
    std::cout << "\n=== Test 3: Command with very short timeout (100ms) ===" << std::endl;

    // 使用非常短的超时，可能会触发超时错误
    auto ping_result = co_await client.command(command_builder.ping())
                           .timeout(std::chrono::milliseconds(100));
    if (ping_result && ping_result.value()) {
        std::cout << "PING command succeeded within 100ms" << std::endl;
    } else if (!ping_result) {
        std::cerr << "PING command failed: " << ping_result.error().message() << std::endl;
        if (ping_result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
            std::cout << "Command timed out as expected!" << std::endl;
        }
    }

    // ==================== 测试4: Pipeline with timeout ====================
    std::cout << "\n=== Test 4: Pipeline with timeout ===" << std::endl;

    RedisCommandBuilder commands;
    commands.reserve(4, 6, 128);
    commands.append("SET", std::array<std::string_view, 2>{"key1", "value1"});
    commands.append("SET", std::array<std::string_view, 2>{"key2", "value2"});
    commands.append("GET", std::array<std::string_view, 1>{"key1"});
    commands.append("GET", std::array<std::string_view, 1>{"key2"});

    // Pipeline也支持超时
    auto pipeline_result = co_await client.batch(commands.commands());
    if (pipeline_result && pipeline_result.value()) {
        auto& values = pipeline_result.value().value();
        std::cout << "Pipeline succeeded, received " << values.size() << " responses" << std::endl;
        for (size_t i = 0; i < values.size(); ++i) {
            std::cout << "  Response " << i << ": ";
            if (values[i].isString()) {
                std::cout << values[i].toString() << std::endl;
            } else if (values[i].isInteger()) {
                std::cout << values[i].toInteger() << std::endl;
            } else {
                std::cout << "(other type)" << std::endl;
            }
        }
    } else if (!pipeline_result) {
        std::cerr << "Pipeline failed: " << pipeline_result.error().message() << std::endl;
    }

    // ==================== 测试5: 多个命令连续执行 ====================
    std::cout << "\n=== Test 5: Multiple commands in sequence ===" << std::endl;

    for (int i = 0; i < 3; ++i) {
        std::string key = "counter_" + std::to_string(i);
        auto incr_result = co_await client.command(command_builder.incr(key))
                               .timeout(std::chrono::seconds(2));

        if (incr_result && incr_result.value()) {
            auto& values = incr_result.value().value();
            if (!values.empty() && values[0].isInteger()) {
                std::cout << "INCR " << key << " = " << values[0].toInteger() << std::endl;
            }
        } else if (!incr_result) {
            std::cerr << "INCR " << key << " failed: " << incr_result.error().message() << std::endl;
        }
    }

    // 关闭连接
    co_await client.close();
    std::cout << "\nConnection closed" << std::endl;
}

/**
 * @brief 测试并发执行多个Redis命令
 */
Task<void> test_concurrent_commands(IOScheduler* scheduler, int client_id)
{
    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder command_builder;

    auto connect_result = co_await client.connect("127.0.0.1", 6379).timeout(std::chrono::seconds(5));
    if (!connect_result) {
        std::cerr << "Client " << client_id << " failed to connect" << std::endl;
        co_return;
    }

    std::cout << "Client " << client_id << " connected" << std::endl;

    // 执行多个命令
    for (int i = 0; i < 5; ++i) {
        std::string key = "client_" + std::to_string(client_id) + "_key_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i);

        auto set_result = co_await client.command(command_builder.set(key, value))
                              .timeout(std::chrono::seconds(3));
        if (set_result && set_result.value()) {
            std::cout << "Client " << client_id << " SET " << key << " succeeded" << std::endl;
        }

        auto get_result = co_await client.command(command_builder.get(key))
                              .timeout(std::chrono::seconds(3));
        if (get_result && get_result.value()) {
            std::cout << "Client " << client_id << " GET " << key << " succeeded" << std::endl;
        }
    }

    co_await client.close();
    std::cout << "Client " << client_id << " closed" << std::endl;
}

int main()
{
    std::cout << "==================================================" << std::endl;
    std::cout << "Redis Client Awaitable with Timeout Support Test" << std::endl;
    std::cout << "==================================================" << std::endl;

    try {
        // 创建运行时
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler" << std::endl;
            return 1;
        }

        // 测试1: 基本超时功能
        std::cout << "\n### Running basic timeout tests ###\n" << std::endl;
        scheduleTask(scheduler, test_redis_client_with_timeout(scheduler));

        // 测试2: 并发客户端
        std::cout << "\n### Running concurrent client tests ###\n" << std::endl;
        for (int i = 0; i < 3; ++i) {
            scheduleTask(scheduler, test_concurrent_commands(scheduler, i));
        }

        // 等待一段时间让测试完成
        std::this_thread::sleep_for(std::chrono::seconds(10));

        runtime.stop();

    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n==================================================" << std::endl;
    std::cout << "All tests completed!" << std::endl;
    std::cout << "==================================================" << std::endl;

    return 0;
}
