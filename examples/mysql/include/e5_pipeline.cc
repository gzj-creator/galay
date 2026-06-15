#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <galay-kernel/core/runtime.h>

#include "common/config.h"
#include "galay-mysql/async/client.h"

using namespace galay::kernel;
using namespace galay::mysql;

namespace
{

struct AsyncState {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    std::string error;
};

Coroutine run(IOScheduler* scheduler, AsyncState* state, const mysql_example::MysqlExampleConfig& cfg)
{
    auto client = AsyncMysqlClientBuilder().scheduler(scheduler).build();

    auto conn_result = co_await client.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!conn_result) {
        state->error = "connect failed: " + conn_result.error().message();
        state->ok.store(false, std::memory_order_relaxed);
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!conn_result->has_value()) {
        state->error = "connect awaitable resumed without value";
        state->ok.store(false, std::memory_order_relaxed);
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    std::vector<std::string_view> sqls;
    sqls.reserve(3);
    sqls.emplace_back("SELECT 11");
    sqls.emplace_back("SELECT 22");
    sqls.emplace_back("SELECT 33");

    auto pipeline_result = co_await client.pipeline(std::span<const std::string_view>(sqls.data(), sqls.size()));
    if (!pipeline_result) {
        state->error = "pipeline failed: " + pipeline_result.error().message();
        state->ok.store(false, std::memory_order_relaxed);
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!pipeline_result->has_value()) {
        state->error = "pipeline awaitable resumed without value";
        state->ok.store(false, std::memory_order_relaxed);
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    const auto& results = pipeline_result->value();
    if (results.size() != sqls.size()) {
        state->error = "pipeline result count mismatch";
        state->ok.store(false, std::memory_order_relaxed);
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].rowCount() == 0) {
            state->error = "pipeline result row count is zero";
            state->ok.store(false, std::memory_order_relaxed);
            co_await client.close();
            state->done.store(true, std::memory_order_release);
            co_return;
        }
        std::cout << "[E5] result[" << i << "] => " << results[i].row(0).getString(0) << std::endl;
    }

    const auto v1 = results[0].row(0).getInt64(0, -1);
    const auto v2 = results[1].row(0).getInt64(0, -1);
    const auto v3 = results[2].row(0).getInt64(0, -1);
    if (v1 != 11 || v2 != 22 || v3 != 33) {
        state->error = "pipeline result value mismatch";
        state->ok.store(false, std::memory_order_relaxed);
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    co_await client.close();
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    const auto cfg = mysql_example::loadMysqlExampleConfig();
    mysql_example::printMysqlExampleConfig(cfg);

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "no IO scheduler" << std::endl;
        runtime.stop();
        return 1;
    }

    AsyncState state;
    if (!scheduleTask(scheduler, run(scheduler, &state, cfg))) {
        std::cerr << "failed to schedule async pipeline example on IO scheduler" << std::endl;
        runtime.stop();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!state.done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "timeout after 20s" << std::endl;
        return 1;
    }
    if (!state.ok.load(std::memory_order_relaxed)) {
        std::cerr << state.error << std::endl;
        return 1;
    }
    return 0;
}
