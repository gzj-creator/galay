#include <iostream>
#include <string>
#include "galay-mysql/sync/mysql_client.h"
#include "config.h"

using namespace galay::mysql;

namespace
{

bool runScalarOne(MysqlClient& client, const char* label)
{
    auto result = client.query("SELECT 1");
    if (!result) {
        std::cerr << label << " SELECT 1 failed: " << result.error().message() << std::endl;
        return false;
    }
    if (result->rowCount() != 1 || result->row(0).getInt64(0, -1) != 1) {
        std::cerr << label << " SELECT 1 returned unexpected result" << std::endl;
        return false;
    }
    return true;
}

std::expected<int64_t, MysqlError> fetchConnectionId(MysqlClient& client)
{
    auto result = client.query("SELECT CONNECTION_ID()");
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->rowCount() != 1) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "CONNECTION_ID returned no row"));
    }
    return result->row(0).getInt64(0, -1);
}

bool killConnection(const mysql_test::DbTestConfig& cfg, int64_t connection_id)
{
    MysqlClient killer;
    auto connect_result = killer.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!connect_result) {
        std::cerr << "killer connect failed: " << connect_result.error().message() << std::endl;
        return false;
    }

    auto kill_result = killer.query("KILL CONNECTION " + std::to_string(connection_id));
    if (!kill_result) {
        std::cerr << "KILL CONNECTION failed: " << kill_result.error().message() << std::endl;
        killer.close();
        return false;
    }

    killer.close();
    return true;
}

bool test_connection_refused_then_recover(const mysql_test::DbTestConfig& cfg)
{
    std::cout << "Testing connection refused then recovery..." << std::endl;

    MysqlConfig bad = MysqlConfig::create(cfg.host,
                                          static_cast<uint16_t>(cfg.port == 3306 ? 3307 : 3306),
                                          cfg.user,
                                          cfg.password,
                                          cfg.database);
    bad.connect_timeout_ms = 200;

    MysqlClient bad_client;
    auto bad_connect = bad_client.connect(bad);
    if (bad_connect) {
        std::cerr << "unexpectedly connected to wrong port " << bad.port << std::endl;
        bad_client.close();
        return false;
    }
    if (bad_connect.error().type() != MYSQL_ERROR_CONNECTION &&
        bad_connect.error().type() != MYSQL_ERROR_TIMEOUT) {
        std::cerr << "wrong-port failure used unexpected error type: "
                  << bad_connect.error().message() << std::endl;
        return false;
    }

    MysqlClient good_client;
    auto good_connect = good_client.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!good_connect) {
        std::cerr << "reconnect after refused connection failed: "
                  << good_connect.error().message() << std::endl;
        return false;
    }

    const bool ok = runScalarOne(good_client, "recovered connection");
    good_client.close();
    return ok;
}

bool test_killed_connection_then_reconnect(const mysql_test::DbTestConfig& cfg)
{
    std::cout << "Testing killed connection then reconnect..." << std::endl;

    MysqlClient victim;
    auto connect_result = victim.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!connect_result) {
        std::cerr << "victim connect failed: " << connect_result.error().message() << std::endl;
        return false;
    }

    auto connection_id = fetchConnectionId(victim);
    if (!connection_id || *connection_id < 0) {
        std::cerr << "failed to fetch victim connection id";
        if (!connection_id) {
            std::cerr << ": " << connection_id.error().message();
        }
        std::cerr << std::endl;
        victim.close();
        return false;
    }

    if (!killConnection(cfg, *connection_id)) {
        victim.close();
        return false;
    }

    auto after_kill = victim.query("SELECT 1");
    if (after_kill) {
        std::cerr << "query unexpectedly succeeded after KILL CONNECTION" << std::endl;
        victim.close();
        return false;
    }
    if (after_kill.error().type() != MYSQL_ERROR_CONNECTION_CLOSED &&
        after_kill.error().type() != MYSQL_ERROR_RECV &&
        after_kill.error().type() != MYSQL_ERROR_SEND) {
        std::cerr << "killed connection failed with unexpected error type: "
                  << after_kill.error().message() << std::endl;
        victim.close();
        return false;
    }
    victim.close();

    MysqlClient fresh;
    auto fresh_connect = fresh.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!fresh_connect) {
        std::cerr << "fresh reconnect after KILL failed: " << fresh_connect.error().message() << std::endl;
        return false;
    }

    const bool ok = runScalarOne(fresh, "fresh connection");
    fresh.close();
    return ok;
}

} // namespace

int main()
{
    std::cout << "=== T12: MySQL Resilience Integration ===" << std::endl;

    const auto cfg = mysql_test::loadDbTestConfig();
    if (const int skip_code = mysql_test::requireDbTestConfigOrSkip(cfg, "T12-MySQLResilience");
        skip_code != 0) {
        return skip_code;
    }
    mysql_test::printDbTestConfig(cfg);

    if (!test_connection_refused_then_recover(cfg)) {
        return 1;
    }
    if (!test_killed_connection_then_reconnect(cfg)) {
        return 1;
    }

    std::cout << "Resilience integration tests PASSED" << std::endl;
    return 0;
}
