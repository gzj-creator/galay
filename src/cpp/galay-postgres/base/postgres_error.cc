#include "postgres_error.h"

#include <utility>

namespace galay::postgres
{

PostgresError::PostgresError(PostgresErrorType type)
    : m_type(type)
{
}

PostgresError::PostgresError(PostgresErrorType type, std::string extra_message)
    : m_extra_message(std::move(extra_message))
    , m_type(type)
{
}

PostgresError::PostgresError(PostgresErrorType type,
                             std::string sql_state,
                             std::string severity,
                             std::string server_message)
    : m_extra_message(std::move(server_message))
    , m_sql_state(std::move(sql_state))
    , m_severity(std::move(severity))
    , m_type(type)
{
}

PostgresErrorType PostgresError::type() const noexcept
{
    return m_type;
}

const std::string& PostgresError::sqlState() const noexcept
{
    return m_sql_state;
}

const std::string& PostgresError::severity() const noexcept
{
    return m_severity;
}

std::string PostgresError::message() const
{
    std::string output;
    switch (m_type) {
    case POSTGRES_ERROR_SUCCESS:           output = "Success"; break;
    case POSTGRES_ERROR_CONNECTION:        output = "Connection error"; break;
    case POSTGRES_ERROR_AUTH:              output = "Authentication error"; break;
    case POSTGRES_ERROR_QUERY:             output = "Query error"; break;
    case POSTGRES_ERROR_PROTOCOL:          output = "Protocol error"; break;
    case POSTGRES_ERROR_TIMEOUT:           output = "Timeout"; break;
    case POSTGRES_ERROR_SEND:              output = "Send error"; break;
    case POSTGRES_ERROR_RECV:              output = "Receive error"; break;
    case POSTGRES_ERROR_CONNECTION_CLOSED: output = "Connection closed"; break;
    case POSTGRES_ERROR_PREPARED_STMT:      output = "Prepared statement error"; break;
    case POSTGRES_ERROR_TRANSACTION:        output = "Transaction error"; break;
    case POSTGRES_ERROR_SERVER:             output = "Server error"; break;
    case POSTGRES_ERROR_INTERNAL:           output = "Internal error"; break;
    case POSTGRES_ERROR_BUFFER_OVERFLOW:    output = "Buffer overflow"; break;
    case POSTGRES_ERROR_INVALID_PARAM:      output = "Invalid parameter"; break;
    default:                                output = "Unknown error"; break;
    }

    if (!m_severity.empty()) {
        output += " (severity=" + m_severity + ")";
    }
    if (!m_sql_state.empty()) {
        output += " (SQLSTATE=" + m_sql_state + ")";
    }
    if (!m_extra_message.empty()) {
        output += ": " + m_extra_message;
    }
    return output;
}

} // namespace galay::postgres
