#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <galay-kernel/core/runtime.h>

#include "common/async_result.h"
#include "common/config.h"

import galay.mongo;

using namespace galay::kernel;
using namespace galay::mongo;

struct RunState
{
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    std::string error;
};

struct AsyncClientConfig
{
    MongoConfig mongo;
    AsyncMongoConfig async;
};

Task<void> run(IOScheduler* scheduler,
               RunState* state,
               AsyncClientConfig cfg)
{
    auto client = AsyncMongoClientBuilder().scheduler(scheduler).config(cfg.async).build();

    const std::expected<bool, MongoError> conn_result =
        mongo_example::unwrapMongoTaskResult(co_await client.connect(cfg.mongo),
                                             MONGO_ERROR_CONNECTION);
    if (!conn_result) {
        state->ok.store(false, std::memory_order_relaxed);
        state->error = "connect failed: " + conn_result.error().message();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    std::vector<MongoDocument> commands;
    commands.reserve(3);

    MongoDocument c1;
    c1.append("ping", int32_t(1));
    commands.push_back(std::move(c1));

    MongoDocument c2;
    c2.append("hello", int32_t(1));
    commands.push_back(std::move(c2));

    MongoDocument c3;
    c3.append("ping", int32_t(1));
    commands.push_back(std::move(c3));

    const std::expected<std::vector<MongoPipelineResponse>, MongoError> pipeline_result =
        mongo_example::unwrapMongoTaskResult(
            co_await client.pipeline(cfg.mongo.database, std::move(commands)),
            MONGO_ERROR_COMMAND);
    if (!pipeline_result) {
        state->ok.store(false, std::memory_order_relaxed);
        state->error = "pipeline failed: " + pipeline_result.error().message();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    for (const auto& item : *pipeline_result) {
        if (item.ok()) {
            std::cout << "requestId=" << item.request_id << " ok" << std::endl;
        } else {
            state->ok.store(false, std::memory_order_relaxed);
            state->error = "requestId=" + std::to_string(item.request_id) +
                           " error=" + item.error->message();
            state->done.store(true, std::memory_order_release);
            co_return;
        }
    }

    co_await client.close();
    state->done.store(true, std::memory_order_release);
}

int main()
{
    const auto mongo_cfg = mongo_example::loadMongoConfigFromEnv();
    const auto async_cfg = mongo_example::loadAsyncMongoConfigFromEnv();

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "No scheduler available" << std::endl;
        runtime.stop();
        return 1;
    }

    RunState state;
    if (!scheduleTask(scheduler, run(scheduler, &state, AsyncClientConfig{mongo_cfg, async_cfg}))) {
        std::cerr << "Failed to schedule async pipeline task" << std::endl;
        runtime.stop();
        return 1;
    }

    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!state.done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }

    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "Async pipeline timeout" << std::endl;
        return 1;
    }

    if (!state.ok.load(std::memory_order_relaxed)) {
        std::cerr << "Async pipeline failed: " << state.error << std::endl;
        return 1;
    }

    std::cout << "Async pipeline example OK" << std::endl;
    return 0;
}
