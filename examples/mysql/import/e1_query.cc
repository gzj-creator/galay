import galay.mysql;

#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <kernel/kernel/runtime.h>
#include "common/config.h"

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

    auto query_result = co_await client.query("SELECT 1");
    if (!query_result) {
        state->error = "query failed: " + query_result.error().message();
        state->ok.store(false, std::memory_order_relaxed);
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!query_result->has_value()) {
        state->error = "query awaitable resumed without value";
        state->ok.store(false, std::memory_order_relaxed);
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    const MysqlResultSet& rs = query_result->value();
    if (rs.rowCount() > 0) {
        std::cout << "[E1-import] SELECT 1 => " << rs.row(0).getString(0) << std::endl;
    } else {
        std::cout << "[E1-import] empty result" << std::endl;
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
        std::cerr << "failed to schedule async query example on IO scheduler" << std::endl;
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
