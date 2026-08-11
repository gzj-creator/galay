/**
 * @file postgres_error.h
 * @brief PostgreSQL client error categories and server diagnostics.
 */

#ifndef GALAY_POSTGRES_ERROR_H
#define GALAY_POSTGRES_ERROR_H

#include <string>

namespace galay::postgres
{

enum PostgresErrorType
{
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

class PostgresError
{
public:
    explicit PostgresError(PostgresErrorType type);
    PostgresError(PostgresErrorType type, std::string extra_message);
    PostgresError(PostgresErrorType type,
                  std::string sql_state,
                  std::string severity,
                  std::string server_message);

    [[nodiscard]] PostgresErrorType type() const noexcept;
    [[nodiscard]] std::string message() const;
    [[nodiscard]] const std::string& sqlState() const noexcept;
    [[nodiscard]] const std::string& severity() const noexcept;

private:
    std::string m_extra_message;
    std::string m_sql_state;
    std::string m_severity;
    PostgresErrorType m_type;
};

} // namespace galay::postgres

#endif // GALAY_POSTGRES_ERROR_H
