#include "common/config.h"
#include <galay-kernel/core/runtime.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

std::optional<int> parsePositiveInt(const char* text)
{
    if (text == nullptr) return std::nullopt;
    try {
        const int value = std::stoi(text);
        if (value <= 0) return std::nullopt;
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
    std::string key_prefix,
    int batch_size)
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

    RedisCommandBuilder commands;
    commands.reserve(static_cast<size_t>(batch_size), static_cast<size_t>(batch_size) * 2U, static_cast<size_t>(batch_size) * 96U);
    for (int i = 0; i < batch_size; ++i) {
        const std::string key = key_prefix + std::to_string(i);
        const std::string value = "value_" + std::to_string(i);
        commands.append("SET", std::array<std::string_view, 2>{key, value});
    }

    auto pipeline_result = co_await client.batch(commands.commands()).timeout(
        std::chrono::seconds(galay::redis::example::kDefaultTimeoutSeconds));
    if (!pipeline_result) {
        std::cerr << "Pipeline failed: " << pipeline_result.error().message() << std::endl;
        (void)co_await client.close();
        finishDemo(*state, 1);
        co_return;
    }
    if (!pipeline_result.value()) {
        std::cerr << "Pipeline returned empty response" << std::endl;
        (void)co_await client.close();
        finishDemo(*state, 1);
        co_return;
    }

    std::cout << "E2 import pipeline responses: " << pipeline_result.value().value().size() << std::endl;

    const std::string sample_key = key_prefix + "0";
    auto sample_result = co_await client.command(command_builder.get(sample_key)).timeout(
        std::chrono::seconds(galay::redis::example::kDefaultTimeoutSeconds));
    if (!sample_result || !sample_result.value()) {
        std::cerr << "Sample GET failed for " << sample_key << std::endl;
        (void)co_await client.close();
        finishDemo(*state, 1);
        co_return;
    }
    const auto& sample_values = sample_result.value().value();
    if (!sample_values.empty() && sample_values[0].isString()) {
        std::cout << "E2 import sample value: " << sample_values[0].toString() << std::endl;
    }

    RedisCommandBuilder cleanup_commands;
    cleanup_commands.reserve(static_cast<size_t>(batch_size), static_cast<size_t>(batch_size), static_cast<size_t>(batch_size) * 64U);
    for (int i = 0; i < batch_size; ++i) {
        const std::string key = key_prefix + std::to_string(i);
        cleanup_commands.append("DEL", std::array<std::string_view, 1>{key});
    }
    auto cleanup_result = co_await client.batch(cleanup_commands.commands()).timeout(
        std::chrono::seconds(galay::redis::example::kDefaultTimeoutSeconds));
    if (!cleanup_result) {
        std::cerr << "Cleanup pipeline failed: " << cleanup_result.error().message() << std::endl;
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
    std::string key_prefix = galay::redis::example::kDefaultPipelinePrefix;
    int batch_size = galay::redis::example::kDefaultPipelineBatchSize;

    if (argc > 1) host = argv[1];
    if (argc > 2) {
        auto parsed_port = parsePort(argv[2]);
        if (!parsed_port) {
            std::cerr << "Invalid port: " << argv[2] << std::endl;
            std::cerr << "Usage: " << argv[0] << " [host] [port] [key_prefix] [batch_size]" << std::endl;
            return 1;
        }
        port = *parsed_port;
    }
    if (argc > 3) key_prefix = argv[3];
    if (argc > 4) {
        auto parsed_batch = parsePositiveInt(argv[4]);
        if (!parsed_batch) {
            std::cerr << "Invalid batch_size: " << argv[4] << std::endl;
            std::cerr << "Usage: " << argv[0] << " [host] [port] [key_prefix] [batch_size]" << std::endl;
            return 1;
        }
        batch_size = *parsed_batch;
    }

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "Failed to get IO scheduler" << std::endl;
        runtime.stop();
        return 1;
    }

    DemoState state;
    scheduleTask(scheduler, runDemo(scheduler, &state, host, port, key_prefix, batch_size));

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
