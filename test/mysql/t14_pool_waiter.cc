#include "config.h"
#include <galay/cpp/galay-mysql/async/conn_pool.h>

#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/concurrency/async_waiter.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace galay::kernel;
using namespace galay::mysql;
using namespace std::chrono_literals;

namespace
{

struct TestState {
    std::atomic<bool> waiter_acquired{false};
    std::atomic<bool> ok{false};
    std::string error;
};

void fail(TestState* state, std::shared_ptr<AsyncWaiter<void>> done, std::string message)
{
    state->error = std::move(message);
    state->ok.store(false, std::memory_order_release);
    done->notify();
}

Task<void> waitingAcquireTask(MysqlConnectionPool* pool,
                              TestState* state,
                              std::shared_ptr<AsyncWaiter<void>> done)
{
    auto acquired = co_await pool->acquire();
    if (!acquired) {
        fail(state, std::move(done), "waiting acquire failed: " + acquired.error().message());
        co_return;
    }
    if (!acquired->has_value()) {
        fail(state, std::move(done), "waiting acquire resumed without value");
        co_return;
    }

    state->waiter_acquired.store(true, std::memory_order_release);
    pool->release(acquired->value());
    state->ok.store(true, std::memory_order_release);
    done->notify();
}

Task<void> runPoolWaiterCase(IOScheduler* scheduler,
                             TestState* state,
                             std::shared_ptr<AsyncWaiter<void>> done,
                             mysql_test::DbTestConfig db_cfg)
{
    MysqlConnectionPoolConfig pool_config;
    pool_config.mysql_config = MysqlConfig::create(db_cfg.host,
                                                   db_cfg.port,
                                                   db_cfg.user,
                                                   db_cfg.password,
                                                   db_cfg.database);
    pool_config.async_config = AsyncMysqlConfig::withTimeout(3s, 3s);
    pool_config.min_connections = 0;
    pool_config.max_connections = 1;

    MysqlConnectionPool pool(scheduler, pool_config);

    auto first = co_await pool.acquire();
    if (!first) {
        fail(state, done, "first acquire failed: " + first.error().message());
        co_return;
    }
    if (!first->has_value()) {
        fail(state, done, "first acquire resumed without value");
        co_return;
    }

    if (!scheduleTask(scheduler, waitingAcquireTask(&pool, state, done))) {
        fail(state, done, "failed to schedule waiter task");
        co_return;
    }

    co_await sleep(100ms);
    if (state->waiter_acquired.load(std::memory_order_acquire)) {
        pool.release(first->value());
        fail(state, done, "waiter acquired before any connection was released");
        co_return;
    }

    pool.release(first->value());

    auto completed = co_await done->wait().timeout(5s);
    if (!completed) {
        state->error = "waiter did not resume after release";
        state->ok.store(false, std::memory_order_release);
        co_return;
    }
}

} // namespace

int main()
{
    std::cout << "=== T14: MySQL Pool Waiter Coroutine Test ===" << std::endl;
    if (const int skip_code = mysql_test::requireIntegrationEnabledOrSkip("T14-MySQLPoolWaiter");
        skip_code != 0) {
        return skip_code;
    }

    const auto db_cfg = mysql_test::loadDbTestConfig();
    if (const int skip_code = mysql_test::requireDbTestConfigOrSkip(db_cfg, "T14-MySQLPoolWaiter");
        skip_code != 0) {
        return skip_code;
    }
    mysql_test::printDbTestConfig(db_cfg);

    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "failed to get IO scheduler" << std::endl;
        return 1;
    }

    TestState state;
    auto done = std::make_shared<AsyncWaiter<void>>();
    auto result = runtime.blockOn(runPoolWaiterCase(scheduler, &state, done, db_cfg));
    runtime.stop();

    if (!result) {
        std::cerr << "runtime failed: " << result.error().message() << std::endl;
        return 1;
    }
    if (!state.ok.load(std::memory_order_acquire)) {
        std::cerr << state.error << std::endl;
        return 1;
    }

    std::cout << "T14-MySQLPoolWaiter PASS" << std::endl;
    return 0;
}
