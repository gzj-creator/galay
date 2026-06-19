#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <galay-kernel/core/runtime.h>

#include "config.h"
#include "galay-mysql/async/client.h"
#include "galay-mysql/sync/mysql_client.h"

using namespace galay::kernel;
using namespace galay::mysql;
using namespace std::chrono_literals;

namespace
{

struct AuthMatrixConfig {
    std::string host;
    uint16_t port = 3306;
    std::string database;
    std::string password;
    std::string native_user = "galay_auth_native";
    std::string caching_user = "galay_auth_caching";
    std::string sha256_user = "galay_auth_sha256";
};

struct AuthCase {
    std::string label;
    std::string user;
    bool supported = true;
};

struct AsyncState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string message;
};

AuthMatrixConfig loadAuthMatrixConfig()
{
    AuthMatrixConfig cfg;
    cfg.host = mysql_test::getEnvOrDefault("GALAY_MYSQL_AUTH_HOST", "GALAY_MYSQL_HOST", cfg.host);
    cfg.port = mysql_test::getEnvPortOrDefault("GALAY_MYSQL_AUTH_PORT", "GALAY_MYSQL_PORT", cfg.port);
    cfg.database = mysql_test::getEnvOrDefault("GALAY_MYSQL_AUTH_DB", "GALAY_MYSQL_DB", cfg.database);
    cfg.password = mysql_test::getEnvOrDefault("GALAY_MYSQL_AUTH_PASSWORD", "GALAY_MYSQL_PASSWORD", cfg.password);
    cfg.native_user = mysql_test::getEnvOrDefault("GALAY_MYSQL_AUTH_NATIVE_USER",
                                                  "MYSQL_AUTH_NATIVE_USER",
                                                  cfg.native_user);
    cfg.caching_user = mysql_test::getEnvOrDefault("GALAY_MYSQL_AUTH_CACHING_USER",
                                                   "MYSQL_AUTH_CACHING_USER",
                                                   cfg.caching_user);
    cfg.sha256_user = mysql_test::getEnvOrDefault("GALAY_MYSQL_AUTH_SHA256_USER",
                                                  "MYSQL_AUTH_SHA256_USER",
                                                  cfg.sha256_user);
    return cfg;
}

int requireAuthMatrixConfigOrSkip(const AuthMatrixConfig& cfg)
{
    if (!cfg.host.empty() && !cfg.database.empty() && !cfg.password.empty()) {
        return 0;
    }

    std::cerr << "T11-MySQLAuthPlugins skipped: set GALAY_MYSQL_AUTH_HOST, "
                 "GALAY_MYSQL_AUTH_PASSWORD, GALAY_MYSQL_AUTH_DB, and optionally "
                 "GALAY_MYSQL_AUTH_PORT/GALAY_MYSQL_AUTH_*_USER. "
                 "Run scripts/mysql/mysql_auth_matrix_setup.sh to create the default users."
              << std::endl;
    return mysql_test::kMysqlTestSkippedExitCode;
}

MysqlConfig makeConfig(const AuthMatrixConfig& cfg,
                       const std::string& user,
                       const std::string& password)
{
    MysqlConfig mysql;
    mysql.host = cfg.host;
    mysql.port = cfg.port;
    mysql.username = user;
    mysql.password = password;
    mysql.database = cfg.database;
    mysql.connect_timeout_ms = 3000;
    return mysql;
}

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

bool verifySelectOne(MysqlClient& client, std::string_view label)
{
    auto result = client.query("SELECT 1");
    if (!result) {
        std::cerr << label << " SELECT failed: " << result.error().message() << std::endl;
        return false;
    }
    if (result->rowCount() != 1 || result->row(0).getInt64(0, -1) != 1) {
        std::cerr << label << " SELECT returned unexpected result" << std::endl;
        return false;
    }
    return true;
}

bool expectSyncConnectSuccess(const AuthMatrixConfig& cfg, const AuthCase& auth)
{
    std::cout << "Sync supported auth plugin case: " << auth.label
              << " user=" << auth.user << std::endl;

    MysqlClient client;
    auto connect_result = client.connect(makeConfig(cfg, auth.user, cfg.password));
    if (!connect_result) {
        std::cerr << auth.label << " connect failed: " << connect_result.error().message() << std::endl;
        return false;
    }

    if (!verifySelectOne(client, auth.label)) {
        client.close();
        return false;
    }

    auto ping = client.ping();
    if (!ping) {
        std::cerr << auth.label << " ping failed: " << ping.error().message() << std::endl;
        client.close();
        return false;
    }
    client.close();

    MysqlClient reconnect_client;
    auto reconnect_result = reconnect_client.connect(makeConfig(cfg, auth.user, cfg.password));
    if (!reconnect_result) {
        std::cerr << auth.label << " reconnect failed: " << reconnect_result.error().message() << std::endl;
        return false;
    }
    reconnect_client.close();
    return true;
}

bool expectSyncWrongPasswordFailure(const AuthMatrixConfig& cfg, const AuthCase& auth)
{
    std::cout << "Sync wrong password case: " << auth.label << std::endl;

    MysqlClient client;
    auto connect_result = client.connect(makeConfig(cfg, auth.user, cfg.password + "_wrong"));
    if (connect_result) {
        std::cerr << auth.label << " wrong password unexpectedly connected" << std::endl;
        client.close();
        return false;
    }
    if (connect_result.error().type() != MYSQL_ERROR_AUTH) {
        std::cerr << auth.label << " wrong password used unexpected error type: "
                  << connect_result.error().message() << std::endl;
        return false;
    }
    return true;
}

bool expectSyncUnsupportedPlugin(const AuthMatrixConfig& cfg, const AuthCase& auth)
{
    std::cout << "Sync unsupported auth plugin case: " << auth.label
              << " user=" << auth.user << std::endl;

    MysqlClient client;
    auto connect_result = client.connect(makeConfig(cfg, auth.user, cfg.password));
    if (connect_result) {
        std::cerr << auth.label << " unexpectedly connected successfully" << std::endl;
        client.close();
        return false;
    }

    const auto error = connect_result.error();
    const auto message = error.message();
    if (error.type() != MYSQL_ERROR_AUTH) {
        std::cerr << auth.label << " failed with wrong error type: " << message << std::endl;
        return false;
    }
    if (!contains(message, "Unsupported auth plugin: sha256_password")) {
        std::cerr << auth.label << " did not reach unsupported-plugin path: " << message << std::endl;
        return false;
    }
    return true;
}

void finish(AsyncState& state, bool ok, std::string message)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.done = true;
    state.ok = ok;
    state.message = std::move(message);
    state.cv.notify_one();
}

Task<void> runAsyncMatrix(IOScheduler* scheduler,
                          AsyncState* state,
                          AuthMatrixConfig cfg,
                          std::vector<AuthCase> auth_cases)
{
    for (const auto& auth : auth_cases) {
        std::cout << "Async auth plugin case: " << auth.label
                  << " user=" << auth.user << std::endl;

        auto client = AsyncMysqlClientBuilder().scheduler(scheduler).build();
        auto connect_result = co_await client.connect(makeConfig(cfg, auth.user, cfg.password));
        if (auth.supported) {
            if (!connect_result) {
                finish(*state,
                       false,
                       auth.label + " async connect failed: " + connect_result.error().message());
                co_return;
            }
            if (!connect_result->has_value()) {
                finish(*state, false, auth.label + " async connect resumed without final value");
                co_return;
            }

            auto result = co_await client.query("SELECT 1");
            if (!result) {
                finish(*state,
                       false,
                       auth.label + " async SELECT failed: " + result.error().message());
                co_return;
            }
            if (!result->has_value() ||
                result->value().rowCount() != 1 ||
                result->value().row(0).getInt64(0, -1) != 1) {
                finish(*state, false, auth.label + " async SELECT returned unexpected result");
                co_return;
            }

            co_await client.close();
            continue;
        }

        if (connect_result && connect_result->has_value()) {
            co_await client.close();
            finish(*state, false, auth.label + " async unsupported plugin unexpectedly connected");
            co_return;
        }
        if (connect_result) {
            finish(*state, false, auth.label + " async unsupported plugin resumed without final result");
            co_return;
        }
        if (connect_result.error().type() != MYSQL_ERROR_AUTH ||
            !contains(connect_result.error().message(), "Unsupported auth plugin: sha256_password")) {
            finish(*state,
                   false,
                   auth.label + " async unsupported plugin returned unexpected error: " +
                       connect_result.error().message());
            co_return;
        }
    }

    finish(*state, true, "async auth matrix passed");
}

bool runAsyncCases(const AuthMatrixConfig& cfg, const std::vector<AuthCase>& auth_cases)
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        std::cerr << "failed to get IO scheduler" << std::endl;
        runtime.stop();
        return false;
    }

    AsyncState state;
    if (!scheduleTask(scheduler, runAsyncMatrix(scheduler, &state, cfg, auth_cases))) {
        std::cerr << "failed to schedule async auth matrix" << std::endl;
        runtime.stop();
        return false;
    }

    std::unique_lock<std::mutex> lock(state.mutex);
    const bool done = state.cv.wait_for(lock, 20s, [&state] { return state.done; });
    const bool ok = state.ok;
    const std::string message = state.message;
    lock.unlock();
    runtime.stop();

    if (!done) {
        std::cerr << "async auth matrix timed out" << std::endl;
        return false;
    }
    if (!ok) {
        std::cerr << message << std::endl;
        return false;
    }

    std::cout << message << std::endl;
    return true;
}

} // namespace

int main()
{
    std::cout << "=== T11: MySQL Auth Plugin Matrix ===" << std::endl;

    if (const int skip_code = mysql_test::requireIntegrationEnabledOrSkip("T11-MySQLAuthPlugins");
        skip_code != 0) {
        return skip_code;
    }

    const auto cfg = loadAuthMatrixConfig();
    if (const int skip_code = requireAuthMatrixConfigOrSkip(cfg); skip_code != 0) {
        return skip_code;
    }

    std::cout << "MySQL auth matrix config: host=" << cfg.host
              << ", port=" << cfg.port
              << ", db=" << cfg.database
              << ", native_user=" << cfg.native_user
              << ", caching_user=" << cfg.caching_user
              << ", sha256_user=" << cfg.sha256_user << std::endl;

    std::vector<AuthCase> auth_cases{
        AuthCase{.label = "mysql_native_password", .user = cfg.native_user, .supported = true},
        AuthCase{.label = "caching_sha2_password", .user = cfg.caching_user, .supported = true},
        AuthCase{.label = "sha256_password", .user = cfg.sha256_user, .supported = false},
    };

    for (const auto& auth : auth_cases) {
        if (auth.supported) {
            if (!expectSyncConnectSuccess(cfg, auth)) {
                return 1;
            }
            if (!expectSyncWrongPasswordFailure(cfg, auth)) {
                return 1;
            }
        } else if (!expectSyncUnsupportedPlugin(cfg, auth)) {
            return 1;
        }
    }

    if (!runAsyncCases(cfg, auth_cases)) {
        return 1;
    }

    std::cout << "Auth plugin matrix PASSED" << std::endl;
    return 0;
}
