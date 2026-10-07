/**
 * @file postgres_config.h
 * @brief PostgreSQL connection and asynchronous operation configuration.
 */

#ifndef GALAY_POSTGRES_CONFIG_H
#define GALAY_POSTGRES_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace galay::postgres
{

struct PostgresConfig
{
    std::string host = "127.0.0.1";
    std::string username;
    std::string password;
    std::string database;
    std::string application_name;
    uint32_t connect_timeout_ms = 5000;
    uint16_t port = 5432;
    bool tcp_no_delay = true;

    static PostgresConfig default_config()
    {
        return {};
    }

    static PostgresConfig create(const std::string& host,
                                 uint16_t port,
                                 const std::string& username,
                                 const std::string& password,
                                 const std::string& database = "")
    {
        PostgresConfig config;
        config.host = host;
        config.port = port;
        config.username = username;
        config.password = password;
        config.database = database;
        return config;
    }
};

struct AsyncPostgresConfig
{
    std::chrono::milliseconds send_timeout{-1};
    std::chrono::milliseconds recv_timeout{-1};
    size_t buffer_size = 16384;
    size_t result_row_reserve_hint = 0;
    bool tcp_no_delay = true;

    [[nodiscard]] bool is_send_timeout_enabled() const
    {
        return send_timeout >= std::chrono::milliseconds(0);
    }

    [[nodiscard]] bool is_recv_timeout_enabled() const
    {
        return recv_timeout >= std::chrono::milliseconds(0);
    }

    static AsyncPostgresConfig with_timeout(std::chrono::milliseconds send,
                                           std::chrono::milliseconds recv)
    {
        AsyncPostgresConfig config;
        config.send_timeout = send;
        config.recv_timeout = recv;
        return config;
    }

    static AsyncPostgresConfig with_send_timeout(std::chrono::milliseconds send)
    {
        AsyncPostgresConfig config;
        config.send_timeout = send;
        return config;
    }

    static AsyncPostgresConfig with_recv_timeout(std::chrono::milliseconds recv)
    {
        AsyncPostgresConfig config;
        config.recv_timeout = recv;
        return config;
    }

    static AsyncPostgresConfig no_timeout()
    {
        return {};
    }
};

} // namespace galay::postgres

#endif // GALAY_POSTGRES_CONFIG_H
