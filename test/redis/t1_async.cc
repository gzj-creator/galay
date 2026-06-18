#include <galay-kernel/core/runtime.h>
#include <iostream>
#include <thread>
#include "galay-redis/async/redis_client.h"

using namespace galay::kernel;
using namespace galay::redis;

Task<void> test_async_redis_client(IOScheduler* scheduler)
{
    std::cout << "Testing asynchronous RedisClient operations..." << std::endl;

    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder builder;

    std::cout << "Connecting to Redis server..." << std::endl;
    auto connect_result = co_await client.connect("127.0.0.1", 6379);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << std::endl;
        co_return;
    }
    std::cout << "Connected successfully!" << std::endl;

    std::cout << "Testing SET operation..." << std::endl;
    auto set_result = co_await client.command(builder.set("test_async_key", "test_async_value"));
    if (!set_result || !set_result.value()) {
        std::cerr << "SET failed: " << set_result.error().message() << std::endl;
        co_return;
    }
    std::cout << "SET operation successful" << std::endl;

    std::cout << "Testing GET operation..." << std::endl;
    auto get_result = co_await client.command(builder.get("test_async_key"));
    if (!get_result || !get_result.value()) {
        std::cerr << "GET failed: " << get_result.error().message() << std::endl;
        co_return;
    }

    const auto& values = get_result.value().value();
    if (!values.empty() && values[0].isString()) {
        std::cout << "GET result: " << values[0].toString() << std::endl;
    } else {
        std::cerr << "GET returned unexpected response" << std::endl;
        co_return;
    }

    std::cout << "Testing DEL operation..." << std::endl;
    auto del_result = co_await client.command(builder.del("test_async_key"));
    if (!del_result || !del_result.value()) {
        std::cerr << "DEL failed: " << del_result.error().message() << std::endl;
        co_return;
    }
    std::cout << "DEL operation successful" << std::endl;

    auto close_result = co_await client.close();
    if (!close_result) {
        std::cerr << "Close failed: " << close_result.error().message() << std::endl;
        co_return;
    }

    std::cout << "Connection closed successfully" << std::endl;
}

int main()
{
    std::cout << "Starting Async Redis client tests..." << std::endl;

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler" << std::endl;
            return 1;
        }

        scheduleTask(scheduler, test_async_redis_client(scheduler));

        std::this_thread::sleep_for(std::chrono::seconds(3));
        runtime.stop();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "All async tests completed." << std::endl;
    return 0;
}
