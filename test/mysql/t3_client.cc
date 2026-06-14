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

// Helper: 执行查询直到完成
// MysqlQueryAwaitable返回 std::expected<std::optional<MysqlResultSet>, MysqlError>
// 成功时应直接返回完整结果
#define MYSQL_CO_QUERY(client, sql, result_var) \
    { \
        auto _r = co_await client.query(sql); \
        if (!_r) { result_var = std::unexpected(_r.error()); } \
        else if (!_r->has_value()) { result_var = std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Query awaitable resumed without value")); } \
        else { result_var = std::move(_r->value()); } \
    }

Coroutine testAsyncMysql(IOScheduler* scheduler, AsyncTestState* state, mysql_test::MysqlTestConfig db_cfg)
{
    std::cout << "Testing asynchronous MySQL operations..." << std::endl;

    auto client = AsyncMysqlClientBuilder().scheduler(scheduler).build();

    // 连接到MySQL服务器
    std::cout << "Connecting to MySQL server..." << std::endl;
    {
        auto connect_awaitable =
            client.connect(db_cfg.host, db_cfg.port, db_cfg.user, db_cfg.password, db_cfg.database);
        auto cr = co_await connect_awaitable;
        if (!cr) {
            state->fail("Connect failed: " + cr.error().message());
            co_return;
        }
        if (!cr->has_value()) {
            state->fail("Connect awaitable resumed without value");
            co_return;
        }
    }
    std::cout << "Connected successfully!" << std::endl;

    // 创建测试表
    std::cout << "Creating test table..." << std::endl;
    {
        auto r = co_await client.query(
            "CREATE TABLE IF NOT EXISTS galay_test ("
            "  id INT AUTO_INCREMENT PRIMARY KEY,"
            "  name VARCHAR(100),"
            "  value INT"
            ") ENGINE=InnoDB"
        );
        if (!r) {
            state->fail("CREATE TABLE failed: " + r.error().message());
            co_return;
        }
        if (!r->has_value()) {
            state->fail("CREATE TABLE awaitable resumed without value");
            co_return;
        }
    }
    std::cout << "Table created." << std::endl;

    // INSERT
    std::cout << "Testing INSERT..." << std::endl;
    {
        auto r = co_await client.query("INSERT INTO galay_test (name, value) VALUES ('test1', 100)");
        if (!r) {
            state->fail("INSERT failed: " + r.error().message());
            co_return;
        }
        if (!r->has_value()) {
            state->fail("INSERT awaitable resumed without value");
            co_return;
        }
        auto& rs = r->value();
        std::cout << "  Affected rows: " << rs.affectedRows() << ", Last insert ID: " << rs.lastInsertId() << std::endl;
    }

    // SELECT
    std::cout << "Testing SELECT..." << std::endl;
    {
        auto r = co_await client.query("SELECT * FROM galay_test");
        if (!r) {
            state->fail("SELECT failed: " + r.error().message());
            co_return;
        }
        if (!r->has_value()) {
            state->fail("SELECT awaitable resumed without value");
            co_return;
        }
        auto& rs = r->value();
        std::cout << "  Columns: " << rs.fieldCount() << ", Rows: " << rs.rowCount() << std::endl;
        for (size_t i = 0; i < rs.fieldCount(); ++i) {
            std::cout << "  Field[" << i << "]: " << rs.field(i).name() << std::endl;
        }
        for (size_t i = 0; i < rs.rowCount(); ++i) {
            auto& row = rs.row(i);
            std::cout << "  Row[" << i << "]:";
            for (size_t j = 0; j < row.size(); ++j) {
                std::cout << " " << row.getString(j, "NULL");
            }
            std::cout << std::endl;
        }
    }

    // PIPELINE
    std::cout << "Testing PIPELINE..." << std::endl;
    {
        std::vector<std::string_view> sqls;
        sqls.reserve(3);
        sqls.emplace_back("SELECT 11");
        sqls.emplace_back("SELECT 22");
        sqls.emplace_back("SELECT 33");

        auto r = co_await client.pipeline(std::span<const std::string_view>(sqls.data(), sqls.size()));
        if (!r) {
            state->fail("PIPELINE failed: " + r.error().message());
            co_return;
        }
        if (!r->has_value()) {
            state->fail("PIPELINE awaitable resumed without value");
            co_return;
        }

        const auto& results = r->value();
        if (results.size() != sqls.size()) {
            state->fail("PIPELINE result count mismatch");
            co_return;
        }

        if (results[0].rowCount() != 1 || results[1].rowCount() != 1 || results[2].rowCount() != 1) {
            state->fail("PIPELINE row count mismatch");
            co_return;
        }

        const auto v1 = results[0].row(0).getInt64(0, -1);
        const auto v2 = results[1].row(0).getInt64(0, -1);
        const auto v3 = results[2].row(0).getInt64(0, -1);
        if (v1 != 11 || v2 != 22 || v3 != 33) {
            state->fail("PIPELINE result value mismatch");
            co_return;
        }
    }

    // UPDATE
    std::cout << "Testing UPDATE..." << std::endl;
    {
        auto r = co_await client.query("UPDATE galay_test SET value = 200 WHERE name = 'test1'");
        if (!r) {
            state->fail("UPDATE failed: " + r.error().message());
            co_return;
        }
        if (!r->has_value()) {
            state->fail("UPDATE awaitable resumed without value");
            co_return;
        }
        std::cout << "  Affected rows: " << r->value().affectedRows() << std::endl;
    }

    // DELETE
    std::cout << "Testing DELETE..." << std::endl;
    {
        auto r = co_await client.query("DELETE FROM galay_test WHERE name = 'test1'");
        if (!r) {
            state->fail("DELETE failed: " + r.error().message());
            co_return;
        }
        if (!r->has_value()) {
            state->fail("DELETE awaitable resumed without value");
            co_return;
        }
        std::cout << "  Affected rows: " << r->value().affectedRows() << std::endl;
    }

    // 清理
    {
        auto _ = co_await client.query("DROP TABLE IF EXISTS galay_test");
        (void)_;
    }

    // 关闭连接
    std::cout << "Closing connection..." << std::endl;
    co_await client.close();
    std::cout << "Connection closed." << std::endl;

    state->pass();
    co_return;
}

int main()
{
    std::cout << "=== T3: Async MySQL Client Tests ===" << std::endl;
    const auto db_cfg = mysql_test::loadMysqlTestConfig();
    if (const int skip_code = mysql_test::requireMysqlTestConfigOrSkip(db_cfg, "T3-AsyncMysqlClient");
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
        if (!scheduleTask(scheduler, testAsyncMysql(scheduler, &state, db_cfg))) {
            std::cerr << "Failed to schedule async MySQL test task on IO scheduler" << std::endl;
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

    std::cout << "All async tests completed." << std::endl;
    return 0;
}
