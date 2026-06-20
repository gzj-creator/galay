#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <galay/cpp/galay-kernel/core/runtime.h>

#include <galay/cpp/galay-mongo/async/client.h>
#include <galay/cpp/galay-mongo/protoc/builder.h>
#include "async_result_helper.h"
#include "config.h"

using namespace galay::kernel;
using namespace galay::mongo;

struct PipelineTestState
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

Task<void> runPipelineTest(IOScheduler* scheduler,
                           PipelineTestState* state,
                           AsyncClientConfig cfg)
{
    auto client = AsyncMongoClientBuilder()
        .scheduler(scheduler)
        .config(cfg.async)
        .build();

    const std::expected<bool, MongoError> conn_result =
        mongo_test::unwrapMongoTaskResult(co_await client.connect(std::move(cfg.mongo)),
                                          MONGO_ERROR_CONNECTION);
    if (!conn_result) {
        state->ok.store(false, std::memory_order_relaxed);
        state->error = "connect failed: " + conn_result.error().message();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    protocol::MongoCommandBuilder commands;
    commands.reserve(3);
    commands.append("ping", int32_t(1));
    commands.append("galayUnknownCommand", int32_t(1));
    commands.append("ping", int32_t(1));

    const std::expected<std::vector<MongoPipelineResponse>, MongoError> pipeline_result =
        mongo_test::unwrapMongoTaskResult(co_await client.pipeline("admin", commands.commands()),
                                          MONGO_ERROR_COMMAND);
    if (!pipeline_result) {
        state->ok.store(false, std::memory_order_relaxed);
        state->error = "pipeline failed: " + pipeline_result.error().message();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    const auto& items = *pipeline_result;
    if (items.size() != 3) {
        state->ok.store(false, std::memory_order_relaxed);
        state->error = "unexpected pipeline response size";
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    size_t ok_count = 0;
    size_t err_count = 0;
    for (const auto& item : items) {
        if (item.request_id <= 0) {
            state->ok.store(false, std::memory_order_relaxed);
            state->error = "invalid request_id in pipeline response";
            state->done.store(true, std::memory_order_release);
            co_return;
        }

        if (item.ok()) {
            ++ok_count;
        } else {
            ++err_count;
        }
    }

    if (ok_count != 2 || err_count != 1) {
        state->ok.store(false, std::memory_order_relaxed);
        state->error = "unexpected per-command success/error distribution";
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    co_await client.close();
    state->done.store(true, std::memory_order_release);
}

int main()
{
    std::cout << "=== T3: Async Mongo Pipeline Tests ===" << std::endl;

    const auto test_cfg = mongo_test::loadMongoTestConfig();
    mongo_test::printMongoTestConfig(test_cfg);

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        std::cerr << "No scheduler available" << std::endl;
        runtime.stop();
        return 1;
    }

    PipelineTestState state;
    if (!scheduleTask(scheduler,
                      runPipelineTest(scheduler,
                                      &state,
                                      AsyncClientConfig{
                                          mongo_test::toMongoConfig(test_cfg),
                                          mongo_test::loadAsyncMongoTestConfig()}))) {
        std::cerr << "Failed to schedule async pipeline task" << std::endl;
        runtime.stop();
        return 1;
    }

    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!state.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
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

    std::cout << "Async pipeline test OK" << std::endl;
    return 0;
}
