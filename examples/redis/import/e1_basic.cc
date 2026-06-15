#include "common/config.h"
#include <galay-kernel/core/runtime.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

import galay.redis;

using namespace galay::kernel;
using namespace galay::redis;

namespace {

struct DemoState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    int code{1};
};

void finishDemo(DemoState& state, int code)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.done = true;
    state.code = code;
    state.cv.notify_one();
}

std::optional<int> parsePort(const char* text)
{
    if (text == nullptr) return std::nullopt;
    try {
        const int value = std::stoi(text);
        if (value <= 0 || value > 65535) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

Task<void> runDemo(
    IOScheduler* scheduler,
    DemoState* state,
    std::string host,
    int port,
    std::string key,
    std::string value)
{
    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder command_builder;

    auto connect_result = co_await client.connect(host, port).timeout(
        std::chrono::seconds(galay::redis::example::kDefaultTimeoutSeconds));
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << std::endl;
        finishDemo(*state, 1);
        co_return;
    }

    auto set_result = co_await client.command(command_builder.set(key, value)).timeout(
        std::chrono::seconds(galay::redis::example::kDefaultTimeoutSeconds));
    if (!set_result) {
        std::cerr << "SET failed: " << set_result.error().message() << std::endl;
        (void)co_await client.close();
        finishDemo(*state, 1);
        co_return;
    }
    if (!set_result.value()) {
        std::cerr << "SET returned empty response" << std::endl;
        (void)co_await client.close();
        finishDemo(*state, 1);
        co_return;
    }

    auto get_result = co_await client.command(command_builder.get(key)).timeout(
        std::chrono::seconds(galay::redis::example::kDefaultTimeoutSeconds));
    if (!get_result) {
        std::cerr << "GET failed: " << get_result.error().message() << std::endl;
        (void)co_await client.close();
        finishDemo(*state, 1);
        co_return;
    }
    if (!get_result.value()) {
        std::cerr << "GET returned empty response" << std::endl;
        (void)co_await client.close();
        finishDemo(*state, 1);
        co_return;
    }

    const auto& values = get_result.value().value();
    if (values.empty() || !values[0].isString()) {
        std::cerr << "GET response is empty or not string" << std::endl;
        (void)co_await client.close();
        finishDemo(*state, 1);
        co_return;
    }
    std::cout << "E1 import demo value: " << values[0].toString() << std::endl;

    auto del_result = co_await client.command(command_builder.del(key)).timeout(
        std::chrono::seconds(galay::redis::example::kDefaultTimeoutSeconds));
    if (!del_result) {
        std::cerr << "DEL failed: " << del_result.error().message() << std::endl;
        (void)co_await client.close();
        finishDemo(*state, 1);
        co_return;
    }

    auto close_result = co_await client.close();
    if (!close_result) {
        std::cerr << "Close failed: " << close_result.error().message() << std::endl;
        finishDemo(*state, 1);
        co_return;
    }

    finishDemo(*state, 0);
}

}  // namespace

int main(int argc, char* argv[])
{
    std::string host = galay::redis::example::kDefaultRedisHost;
    int port = galay::redis::example::kDefaultRedisPort;
    std::string key = galay::redis::example::kDefaultDemoKey;
    std::string value = galay::redis::example::kDefaultDemoValue;

    if (argc > 1) host = argv[1];
    if (argc > 2) {
        auto parsed_port = parsePort(argv[2]);
        if (!parsed_port) {
            std::cerr << "Invalid port: " << argv[2] << std::endl;
            std::cerr << "Usage: " << argv[0] << " [host] [port] [key] [value]" << std::endl;
            return 1;
        }
        port = *parsed_port;
    }
    if (argc > 3) key = argv[3];
    if (argc > 4) value = argv[4];

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "Failed to get IO scheduler" << std::endl;
        runtime.stop();
        return 1;
    }

    DemoState state;
    scheduleTask(scheduler, runDemo(scheduler, &state, host, port, key, value));

    std::unique_lock<std::mutex> lock(state.mutex);
    const bool finished = state.cv.wait_for(lock, std::chrono::seconds(15), [&]() {
        return state.done;
    });

    runtime.stop();

    if (!finished) {
        std::cerr << "Demo timeout after 15s" << std::endl;
        return 1;
    }
    return state.code;
}
