#include <iostream>
#include <atomic>
#include <kernel/kernel/runtime.h>
#include "mysql/async/client.h"
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

inline void markFailure(AsyncTestState* state, const std::string& msg) {
    std::cerr << msg << std::endl;
    state->fail(msg);
}

// Helper macros for async MySQL operations
#define MYSQL_CO_CONNECT(client, host, port, user, pass, db) \
    { \
        auto _r = co_await client.connect(host, port, user, pass, db); \
        if (!_r) { markFailure(state, "Connect failed: " + _r.error().message()); co_return; } \
        if (!_r->has_value()) { markFailure(state, "Connect awaitable resumed without value"); co_return; } \
    }

#define MYSQL_CO_QUERY(client, sql, result_var) \
    { \
        auto _r = co_await client.query(sql); \
        if (!_r) { markFailure(state, std::string("Query failed [") + sql + "]: " + _r.error().message()); co_return; } \
        if (!_r->has_value()) { markFailure(state, std::string("Query awaitable resumed without value [") + sql + "]"); co_return; } \
        if (_r) { result_var = std::move(_r->value()); } \
    }

#define MYSQL_CO_QUERY_VOID(client, sql) \
    { \
        auto _r = co_await client.query(sql); \
        if (!_r) { markFailure(state, std::string("Query failed [") + sql + "]: " + _r.error().message()); co_return; } \
        if (!_r->has_value()) { markFailure(state, std::string("Query awaitable resumed without value [") + sql + "]"); co_return; } \
    }

Coroutine testTransaction(IOScheduler* scheduler, AsyncTestState* state, mysql_test::MysqlTestConfig db_cfg)
{
    std::cout << "Testing MySQL transactions..." << std::endl;

    auto client = AsyncMysqlClientBuilder().scheduler(scheduler).build();

    MYSQL_CO_CONNECT(client, db_cfg.host, db_cfg.port, db_cfg.user, db_cfg.password, db_cfg.database);
    std::cout << "Connected." << std::endl;

    // 创建测试表
    MYSQL_CO_QUERY_VOID(client, "CREATE TABLE IF NOT EXISTS galay_tx_test (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100), balance INT DEFAULT 0) ENGINE=InnoDB");
    MYSQL_CO_QUERY_VOID(client, "TRUNCATE TABLE galay_tx_test");
    MYSQL_CO_QUERY_VOID(client, "INSERT INTO galay_tx_test (name, balance) VALUES ('Alice', 1000)");
    MYSQL_CO_QUERY_VOID(client, "INSERT INTO galay_tx_test (name, balance) VALUES ('Bob', 500)");

    // 测试事务提交
    std::cout << "Testing COMMIT..." << std::endl;
    {
        MYSQL_CO_QUERY_VOID(client, "BEGIN");
        MYSQL_CO_QUERY_VOID(client, "UPDATE galay_tx_test SET balance = balance - 100 WHERE name = 'Alice'");
        MYSQL_CO_QUERY_VOID(client, "UPDATE galay_tx_test SET balance = balance + 100 WHERE name = 'Bob'");
        MYSQL_CO_QUERY_VOID(client, "COMMIT");
        std::cout << "  Transaction committed." << std::endl;
    }

    // 验证结果
    {
        std::expected<MysqlResultSet, MysqlError> sr = std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, ""));
        MYSQL_CO_QUERY(client, "SELECT name, balance FROM galay_tx_test ORDER BY name", sr);
        if (sr) {
            for (size_t i = 0; i < sr->rowCount(); ++i) {
                std::cout << "  " << sr->row(i).getString(0) << ": " << sr->row(i).getString(1) << std::endl;
            }
        }
    }

    // 测试事务回滚
    std::cout << "Testing ROLLBACK..." << std::endl;
    {
        MYSQL_CO_QUERY_VOID(client, "BEGIN");
        MYSQL_CO_QUERY_VOID(client, "UPDATE galay_tx_test SET balance = 0 WHERE name = 'Alice'");
        MYSQL_CO_QUERY_VOID(client, "ROLLBACK");
        std::cout << "  Transaction rolled back." << std::endl;
    }

    // 验证回滚后数据不变
    {
        std::expected<MysqlResultSet, MysqlError> sr = std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, ""));
        MYSQL_CO_QUERY(client, "SELECT name, balance FROM galay_tx_test WHERE name = 'Alice'", sr);
        if (sr && sr->rowCount() > 0) {
            std::cout << "  Alice balance after rollback: " << sr->row(0).getString(1) << std::endl;
        }
    }

    // 清理
    MYSQL_CO_QUERY_VOID(client, "DROP TABLE IF EXISTS galay_tx_test");
    co_await client.close();

    std::cout << "Transaction tests completed." << std::endl;
    state->pass();
    co_return;
}

int main()
{
    std::cout << "=== T6: Transaction Tests ===" << std::endl;
    const auto db_cfg = mysql_test::loadMysqlTestConfig();
    if (const int skip_code = mysql_test::requireMysqlTestConfigOrSkip(db_cfg, "t6_transaction");
        skip_code != 0) {
        return skip_code;
    }
    mysql_test::printMysqlTestConfig(db_cfg);

    try {
        Runtime runtime;
        runtime.start();
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) { std::cerr << "No scheduler" << std::endl; return 1; }
        AsyncTestState state;
        if (!scheduleTask(scheduler, testTransaction(scheduler, &state, db_cfg))) {
            std::cerr << "Failed to schedule transaction test task on IO scheduler" << std::endl;
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

    std::cout << "All transaction tests completed." << std::endl;
    return 0;
}
