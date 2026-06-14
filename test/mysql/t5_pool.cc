#include <iostream>
#include <atomic>
#include <kernel/kernel/runtime.h>
#include "mysql/async/conn_pool.h"
#include "config.h"

using namespace galay::kernel;
using namespace galay::mysql;

struct AsyncTestState {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    std::string error;

    void fail(std::string msg) {
        error = std::move(msg);
        ok.store(false, std::memory_order_relaxed);
        done.store(true, std::memory_order_release);
    }

    void pass() {
        done.store(true, std::memory_order_release);
    }
};

Coroutine testConnectionPool(IOScheduler* scheduler, AsyncTestState* state, mysql_test::MysqlTestConfig db_cfg)
{
    std::cout << "Testing MySQL connection pool..." << std::endl;

    MysqlConfig config = MysqlConfig::create(db_cfg.host, db_cfg.port, db_cfg.user, db_cfg.password, db_cfg.database);
    MysqlConnectionPoolConfig pool_config;
    pool_config.mysql_config = config;
    pool_config.async_config = AsyncMysqlConfig::noTimeout();
    pool_config.min_connections = 2;
    pool_config.max_connections = 5;
    MysqlConnectionPool pool(scheduler, pool_config);

    // 获取连接
    std::cout << "Acquiring connection..." << std::endl;
    AsyncMysqlClient* client = nullptr;
    {
        auto ar = co_await pool.acquire();
        if (!ar) {
            state->fail("Acquire failed: " + ar.error().message());
            co_return;
        }
        if (!ar->has_value()) {
            state->fail("Acquire awaitable resumed without value");
            co_return;
        }
        client = ar->value();
    }
    std::cout << "Connection acquired, pool size: " << pool.size() << std::endl;

    // 执行查询
    {
        auto qr = co_await client->query("SELECT 1 AS test_col");
        if (qr && qr->has_value()) {
            std::cout << "  Query result: " << qr->value().row(0).getString(0) << std::endl;
        } else if (!qr) {
            state->fail("Query failed: " + qr.error().message());
            co_return;
        } else {
            state->fail("Query awaitable resumed without value");
            co_return;
        }
    }

    // 归还连接
    pool.release(client);
    std::cout << "Connection released." << std::endl;

    // 再次获取（应该复用已有连接，不需要循环）
    {
        auto ar2 = co_await pool.acquire();
        if (!ar2) {
            state->fail("Second acquire failed: " + ar2.error().message());
            co_return;
        }
        if (!ar2->has_value()) {
            state->fail("Second acquire awaitable resumed without value");
            co_return;
        }
        std::cout << "Connection reused, pool size: " << pool.size() << std::endl;
        pool.release(ar2->value());
    }

    std::cout << "Connection pool test completed." << std::endl;
    state->pass();
    co_return;
}

int main()
{
    std::cout << "=== T5: Connection Pool Tests ===" << std::endl;
    const auto db_cfg = mysql_test::loadMysqlTestConfig();
    if (const int skip_code = mysql_test::requireMysqlTestConfigOrSkip(db_cfg, "T5-ConnectionPool");
        skip_code != 0) {
        return skip_code;
    }
    mysql_test::printMysqlTestConfig(db_cfg);

    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler" << std::endl;
            return 1;
        }

        AsyncTestState state;
        if (!scheduleTask(scheduler, testConnectionPool(scheduler, &state, db_cfg))) {
            std::cerr << "Failed to schedule connection pool test task on IO scheduler" << std::endl;
            runtime.stop();
            return 1;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!state.done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        runtime.stop();

        if (!state.done.load(std::memory_order_acquire)) {
            std::cerr << "Test timeout after 20s" << std::endl;
            return 1;
        }
        if (!state.ok.load(std::memory_order_relaxed)) {
            std::cerr << state.error << std::endl;
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "All connection pool tests completed." << std::endl;
    return 0;
}
