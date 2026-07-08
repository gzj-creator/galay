#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include "common/config.h"
#include <galay/cpp/galay-mysql/async/conn_pool.h>

using namespace galay::kernel;
using namespace galay::mysql;

namespace
{

struct AsyncState {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    std::string error;
};

Task<void> run(IOScheduler* scheduler, AsyncState* state, const mysql_example::DbExampleConfig& env_cfg)
{
    MysqlConfig cfg;
    cfg.host = env_cfg.host;
    cfg.port = env_cfg.port;
    cfg.username = env_cfg.user;
    cfg.password = env_cfg.password;
    cfg.database = env_cfg.database;

    MysqlConnectionPoolConfig pool_cfg;
    pool_cfg.mysql_config = cfg;
    pool_cfg.async_config = AsyncMysqlConfig::withTimeout(
        std::chrono::milliseconds(3000), std::chrono::milliseconds(5000));
    pool_cfg.min_connections = 1;
    pool_cfg.max_connections = 8;

    MysqlConnectionPool pool(scheduler, pool_cfg);

    auto acq = co_await pool.acquire();
    if (!acq) {
        state->error = "acquire failed: " + acq.error().message();
        state->ok.store(false, std::memory_order_relaxed);
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!acq->has_value()) {
        state->error = "acquire awaitable resumed without value";
        state->ok.store(false, std::memory_order_relaxed);
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    AsyncMysqlClient<>* client = acq->value();

    auto query_res = co_await client->query("SELECT CONNECTION_ID()");
    if (!query_res) {
        state->error = "query failed: " + query_res.error().message();
        state->ok.store(false, std::memory_order_relaxed);
        pool.release(client);
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!query_res->has_value()) {
        state->error = "query awaitable resumed without value";
        state->ok.store(false, std::memory_order_relaxed);
        pool.release(client);
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    const auto& rs = query_res->value();
    if (rs.rowCount() > 0) {
        std::cout << "[E3] CONNECTION_ID() => " << rs.row(0).getString(0) << std::endl;
    }

    pool.release(client);
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    const auto cfg = mysql_example::loadDbExampleConfig();
    mysql_example::printDbExampleConfig(cfg);

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
        std::cerr << "failed to schedule async pool example on IO scheduler" << std::endl;
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
