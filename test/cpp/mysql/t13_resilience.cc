#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <galay/cpp/galay-mysql/sync/mysql_client.h>
#include "config.h"

using namespace galay::mysql;

namespace
{

bool run_scalar_one(MysqlClient& client, const char* label)
{
    auto result = client.query("SELECT 1");
    if (!result) {
        std::cerr << label << " SELECT 1 failed: " << result.error().message() << std::endl;
        return false;
    }
    if (result->row_count() != 1 || result->row(0).get_int64(0, -1) != 1) {
        std::cerr << label << " SELECT 1 returned unexpected result" << std::endl;
        return false;
    }
    return true;
}

std::expected<int64_t, MysqlError> fetch_connection_id(MysqlClient& client)
{
    auto result = client.query("SELECT CONNECTION_ID()");
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->row_count() != 1) {
        return std::unexpected(MysqlError(MYSQL_ERROR_PROTOCOL, "CONNECTION_ID returned no row"));
    }
    return result->row(0).get_int64(0, -1);
}

bool kill_connection(const mysql_test::DbTestConfig& cfg, int64_t connection_id)
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

    // Keep a loopback port reserved without listening so no other server can answer.
    const int reserved_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (reserved_fd < 0) {
        std::cerr << "reserve socket failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    struct ReservedSocket {
        int fd;
        ~ReservedSocket()
        {
            if (::close(fd) != 0) {
                std::cerr << "close reserved socket failed: " << std::strerror(errno) << std::endl;
                std::abort();
            }
        }
    } reserved{reserved_fd};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(reserved_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "reserve port failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    socklen_t address_size = sizeof(address);
    if (::getsockname(reserved_fd, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
        std::cerr << "read reserved port failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    MysqlConfig bad = MysqlConfig::create("127.0.0.1",
                                          ntohs(address.sin_port),
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

    const bool ok = run_scalar_one(good_client, "recovered connection");
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

    auto connection_id = fetch_connection_id(victim);
    if (!connection_id || *connection_id < 0) {
        std::cerr << "failed to fetch victim connection id";
        if (!connection_id) {
            std::cerr << ": " << connection_id.error().message();
        }
        std::cerr << std::endl;
        victim.close();
        return false;
    }

    if (!kill_connection(cfg, *connection_id)) {
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

    const bool ok = run_scalar_one(fresh, "fresh connection");
    fresh.close();
    return ok;
}

} // namespace

int main()
{
    std::cout << "=== T12: MySQL Resilience Integration ===" << std::endl;

    if (const int skip_code = mysql_test::require_integration_enabled_or_skip("T12-MySQLResilience");
        skip_code != 0) {
        return skip_code;
    }

    const auto cfg = mysql_test::load_db_test_config();
    if (const int skip_code = mysql_test::require_db_test_config_or_skip(cfg, "T12-MySQLResilience");
        skip_code != 0) {
        return skip_code;
    }
    mysql_test::print_db_test_config(cfg);

    if (!test_connection_refused_then_recover(cfg)) {
        return 1;
    }
    if (!test_killed_connection_then_reconnect(cfg)) {
        return 1;
    }

    std::cout << "Resilience integration tests PASSED" << std::endl;
    return 0;
}
