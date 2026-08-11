#include <galay/cpp/galay-postgres/base/postgres_config.h>
#include <galay/cpp/galay-postgres/base/postgres_error.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace std::chrono_literals;

namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testDefaultsAndFactories()
{
    using namespace galay::postgres;

    const PostgresConfig defaults = PostgresConfig::defaultConfig();
    require(defaults.host == "127.0.0.1", "default host mismatch");
    require(defaults.port == 5432, "default port mismatch");
    require(defaults.username.empty(), "default username must be empty");
    require(defaults.password.empty(), "default password must be empty");
    require(defaults.database.empty(), "default database must be empty");
    require(defaults.application_name.empty(), "default application name must be empty");
    require(defaults.connect_timeout_ms == 5000, "default connect timeout mismatch");
    require(defaults.tcp_no_delay, "TCP_NODELAY must be enabled by default");

    const PostgresConfig created = PostgresConfig::create(
        "db.internal", 5544, "reader", "secret", "analytics");
    require(created.host == "db.internal", "factory host mismatch");
    require(created.port == 5544, "factory port mismatch");
    require(created.username == "reader", "factory username mismatch");
    require(created.password == "secret", "factory password mismatch");
    require(created.database == "analytics", "factory database mismatch");
    require(created.connect_timeout_ms == 5000, "factory must preserve timeout default");

    const AsyncPostgresConfig async_defaults{};
    require(!async_defaults.isSendTimeoutEnabled(), "send timeout must be disabled by default");
    require(!async_defaults.isRecvTimeoutEnabled(), "receive timeout must be disabled by default");
    require(async_defaults.buffer_size == 16384, "default receive buffer mismatch");
    require(async_defaults.result_row_reserve_hint == 0, "default row reserve mismatch");
    require(async_defaults.tcp_no_delay, "async TCP_NODELAY must be enabled by default");

    const AsyncPostgresConfig timed = AsyncPostgresConfig::withTimeout(250ms, 750ms);
    require(timed.isSendTimeoutEnabled() && timed.send_timeout == 250ms,
            "send timeout factory mismatch");
    require(timed.isRecvTimeoutEnabled() && timed.recv_timeout == 750ms,
            "receive timeout factory mismatch");

    const AsyncPostgresConfig untimed = AsyncPostgresConfig::noTimeout();
    require(!untimed.isSendTimeoutEnabled() && !untimed.isRecvTimeoutEnabled(),
            "noTimeout must disable both directions");
}

void testErrorMappingAndServerMetadata()
{
    using namespace galay::postgres;

    constexpr std::array<PostgresErrorType, 15> kTypes{
        POSTGRES_ERROR_SUCCESS,
        POSTGRES_ERROR_CONNECTION,
        POSTGRES_ERROR_AUTH,
        POSTGRES_ERROR_QUERY,
        POSTGRES_ERROR_PROTOCOL,
        POSTGRES_ERROR_TIMEOUT,
        POSTGRES_ERROR_SEND,
        POSTGRES_ERROR_RECV,
        POSTGRES_ERROR_CONNECTION_CLOSED,
        POSTGRES_ERROR_PREPARED_STMT,
        POSTGRES_ERROR_TRANSACTION,
        POSTGRES_ERROR_SERVER,
        POSTGRES_ERROR_INTERNAL,
        POSTGRES_ERROR_BUFFER_OVERFLOW,
        POSTGRES_ERROR_INVALID_PARAM,
    };

    for (PostgresErrorType type : kTypes) {
        const PostgresError error(type);
        require(error.type() == type, "error type accessor mismatch");
        require(!error.message().empty(), "every public error type needs a message");
        require(error.sqlState().empty(), "local errors must not have SQLSTATE");
        require(error.severity().empty(), "local errors must not have severity");
    }

    const PostgresError detailed(POSTGRES_ERROR_PROTOCOL, "invalid message length");
    require(detailed.message().find("invalid message length") != std::string::npos,
            "detail must be retained in formatted error message");

    const PostgresError server(POSTGRES_ERROR_SERVER,
                               "42601",
                               "ERROR",
                               "syntax error at or near SELECT");
    require(server.sqlState() == "42601", "server SQLSTATE mismatch");
    require(server.severity() == "ERROR", "server severity mismatch");
    require(server.message().find("42601") != std::string::npos,
            "formatted server error must include SQLSTATE");
    require(server.message().find("syntax error") != std::string::npos,
            "formatted server error must include server message");

    const PostgresError unknown(static_cast<PostgresErrorType>(999));
    require(unknown.message().find("Unknown") != std::string::npos,
            "unknown enum values need a deterministic message");
}

} // namespace

int main()
{
    testDefaultsAndFactories();
    testErrorMappingAndServerMetadata();
    return EXIT_SUCCESS;
}
