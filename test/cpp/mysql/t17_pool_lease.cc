#include "config.h"
#include <galay/cpp/galay-mysql/async/conn_pool.h>

#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>

using namespace galay::kernel;
using namespace galay::mysql;
using namespace std::chrono_literals;

namespace
{

struct TestState {
    std::atomic<bool> ok{false};
    std::string error;
};

bool expect_select_one(const MysqlResultSet& result)
{
    return result.row_count() == 1 && result.row(0).get_string(0) == "1";
}

Task<void> run_pool_lease_case(IOScheduler* scheduler, TestState* state, mysql_test::DbTestConfig db_cfg)
{
    MysqlConnectionPoolConfig pool_config;
    pool_config.mysql_config = MysqlConfig::create(db_cfg.host,
                                                   db_cfg.port,
                                                   db_cfg.user,
                                                   db_cfg.password,
                                                   db_cfg.database);
    pool_config.async_config = AsyncMysqlConfig::with_timeout(3s, 3s);
    pool_config.min_connections = 0;
    pool_config.max_connections = 1;

    MysqlConnectionPool pool(scheduler, pool_config);

    {
        auto first = co_await pool.acquire_lease();
        if (!first) {
            state->error = "first lease acquire failed: " + first.error().message();
            co_return;
        }
        if (!first->has_value()) {
            state->error = "first lease acquire resumed without value";
            co_return;
        }

        auto result = co_await first->value()->query("SELECT 1");
        if (!result) {
            state->error = "first query failed: " + result.error().message();
            co_return;
        }
        if (!result->has_value() || !expect_select_one(result->value())) {
            state->error = "first query returned unexpected result";
            co_return;
        }
    }

    auto second = co_await pool.acquire_lease();
    if (!second) {
        state->error = "second lease acquire failed: " + second.error().message();
        co_return;
    }
    if (!second->has_value()) {
        state->error = "second lease acquire resumed without value";
        co_return;
    }

    auto result = co_await second->value()->query("SELECT 1");
    if (!result) {
        state->error = "second query failed: " + result.error().message();
        co_return;
    }
    if (!result->has_value() || !expect_select_one(result->value())) {
        state->error = "second query returned unexpected result";
        co_return;
    }

    std::cout << "T16-MySQLPoolLease second acquire succeeded after lease scope." << std::endl;
    state->ok.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    std::cout << "=== T16: MySQL Pool Lease ===" << std::endl;
    if (const int skip_code = mysql_test::require_integration_enabled_or_skip("T16-MySQLPoolLease");
        skip_code != 0) {
        return skip_code;
    }

    const auto db_cfg = mysql_test::load_db_test_config();
    if (const int skip_code = mysql_test::require_db_test_config_or_skip(db_cfg, "T16-MySQLPoolLease");
        skip_code != 0) {
        return skip_code;
    }
    mysql_test::print_db_test_config(db_cfg);

    Runtime runtime = RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();
    runtime.start();

    auto* scheduler = runtime.get_next_io_scheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "failed to get IO scheduler" << std::endl;
        return 1;
    }

    TestState state;
    auto runtime_result = runtime.block_on_io(run_pool_lease_case(scheduler, &state, db_cfg));
    runtime.stop();

    if (!runtime_result) {
        std::cerr << "runtime failed: " << runtime_result.error().message() << std::endl;
        return 1;
    }
    if (!state.ok.load(std::memory_order_acquire)) {
        std::cerr << state.error << std::endl;
        return 1;
    }

    std::cout << "T16-MySQLPoolLease PASS" << std::endl;
    return 0;
}
