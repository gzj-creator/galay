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

#define MYSQL_CO_PREPARE(client, sql, result_var) \
    { \
        auto _r = co_await client.prepare(sql); \
        if (!_r) { markFailure(state, std::string("PREPARE failed [") + sql + "]: " + _r.error().message()); co_return; } \
        if (!_r->has_value()) { markFailure(state, std::string("PREPARE awaitable resumed without value [") + sql + "]"); co_return; } \
        if (_r && _r->has_value()) { result_var = std::move(_r->value()); } \
    }

#define MYSQL_CO_EXECUTE(client, stmt_id, params, result_var) \
    { \
        auto _r = co_await client.stmtExecute(stmt_id, params); \
        if (!_r) { markFailure(state, std::string("EXECUTE failed [stmt=") + std::to_string(stmt_id) + "]: " + _r.error().message()); co_return; } \
        if (!_r->has_value()) { markFailure(state, std::string("EXECUTE awaitable resumed without value [stmt=") + std::to_string(stmt_id) + "]"); co_return; } \
        if (_r) { result_var = std::move(_r->value()); } \
    }

Coroutine testPreparedStatement(IOScheduler* scheduler, AsyncTestState* state, mysql_test::MysqlTestConfig db_cfg)
{
    std::cout << "Testing MySQL prepared statements..." << std::endl;

    auto client = AsyncMysqlClientBuilder().scheduler(scheduler).build();

    MYSQL_CO_CONNECT(client, db_cfg.host, db_cfg.port, db_cfg.user, db_cfg.password, db_cfg.database);
    std::cout << "Connected." << std::endl;

    // 创建测试表
    MYSQL_CO_QUERY_VOID(client, "CREATE TABLE IF NOT EXISTS galay_stmt_test (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100), age INT) ENGINE=InnoDB");
    MYSQL_CO_QUERY_VOID(client, "TRUNCATE TABLE galay_stmt_test");

    // 准备INSERT语句
    std::cout << "Testing PREPARE..." << std::endl;
    std::optional<MysqlPrepareAwaitable::PrepareResult> prepare_result;
    MYSQL_CO_PREPARE(client, "INSERT INTO galay_stmt_test (name, age) VALUES (?, ?)", prepare_result);

    if (prepare_result) {
        auto& pr = prepare_result.value();
        std::cout << "  Statement ID: " << pr.statement_id
                  << ", Params: " << pr.num_params
                  << ", Columns: " << pr.num_columns << std::endl;

        // 执行预处理语句
        std::cout << "Testing EXECUTE..." << std::endl;
        {
            std::vector<std::optional<std::string>> params1 = {"Alice", "25"};
            std::expected<MysqlResultSet, MysqlError> er = std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "init"));
            MYSQL_CO_EXECUTE(client, pr.statement_id, params1, er);
            std::cout << "  Inserted, affected rows: " << er->affectedRows() << std::endl;
        }

        {
            std::vector<std::optional<std::string>> params2 = {"Bob", "30"};
            std::expected<MysqlResultSet, MysqlError> er = std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "init"));
            MYSQL_CO_EXECUTE(client, pr.statement_id, params2, er);
            std::cout << "  Inserted, affected rows: " << er->affectedRows() << std::endl;
        }

        // 测试NULL参数
        std::cout << "Testing NULL parameter..." << std::endl;
        {
            std::vector<std::optional<std::string>> params3 = {"Charlie", std::nullopt};
            std::expected<MysqlResultSet, MysqlError> er = std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "init"));
            MYSQL_CO_EXECUTE(client, pr.statement_id, params3, er);
            std::cout << "  Inserted with NULL, affected rows: " << er->affectedRows() << std::endl;
        }
    }

    // 验证数据
    std::cout << "Verifying data..." << std::endl;
    {
        std::expected<MysqlResultSet, MysqlError> sr = std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "init"));
        MYSQL_CO_QUERY(client, "SELECT * FROM galay_stmt_test ORDER BY id", sr);
        std::cout << "  Total rows: " << sr->rowCount() << std::endl;
        for (size_t i = 0; i < sr->rowCount(); ++i) {
            auto& row = sr->row(i);
            std::cout << "  [" << row.getString(0) << "] "
                      << row.getString(1) << " - age: "
                      << row.getString(2, "NULL") << std::endl;
        }
    }

    // 准备SELECT语句
    std::cout << "Testing PREPARE SELECT..." << std::endl;
    {
        std::optional<MysqlPrepareAwaitable::PrepareResult> prep_sel;
        MYSQL_CO_PREPARE(client, "SELECT * FROM galay_stmt_test WHERE name = ?", prep_sel);
        if (prep_sel) {
            std::cout << "  Statement ID: " << prep_sel->statement_id << std::endl;
            std::vector<std::optional<std::string>> params = {"Alice"};
            std::expected<MysqlResultSet, MysqlError> er = std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "init"));
            MYSQL_CO_EXECUTE(client, prep_sel->statement_id, params, er);
            std::cout << "  Found " << er->rowCount() << " rows for Alice" << std::endl;
        }
    }

    // 清理
    MYSQL_CO_QUERY_VOID(client, "DROP TABLE IF EXISTS galay_stmt_test");
    co_await client.close();

    std::cout << "Prepared statement tests completed." << std::endl;
    state->pass();
    co_return;
}

int main()
{
    std::cout << "=== T7: Prepared Statement Tests ===" << std::endl;
    const auto db_cfg = mysql_test::loadMysqlTestConfig();
    if (const int skip_code = mysql_test::requireMysqlTestConfigOrSkip(db_cfg, "T7-PreparedStatement");
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
        if (!scheduleTask(scheduler, testPreparedStatement(scheduler, &state, db_cfg))) {
            std::cerr << "Failed to schedule prepared statement test task on IO scheduler" << std::endl;
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

    std::cout << "All prepared statement tests completed." << std::endl;
    return 0;
}
