#include "mysql_error.h"

namespace galay::mysql
{

MysqlError::MysqlError(MysqlErrorType type)
    : m_type(type)
{
}

MysqlError::MysqlError(MysqlErrorType type, std::string extra_msg)
    : m_type(type), m_extra_msg(std::move(extra_msg))
{
}

MysqlError::MysqlError(MysqlErrorType type, uint16_t server_errno, std::string server_msg)
    : m_type(type), m_server_errno(server_errno), m_extra_msg(std::move(server_msg))
{
}

MysqlErrorType MysqlError::type() const
{
    return m_type;
}

uint16_t MysqlError::serverErrno() const
{
    return m_server_errno;
}

std::string MysqlError::message() const
{
    std::string base;
    switch (m_type) {
    case MYSQL_ERROR_SUCCESS:          base = "Success"; break;
    case MYSQL_ERROR_CONNECTION:       base = "Connection error"; break;
    case MYSQL_ERROR_AUTH:             base = "Authentication error"; break;
    case MYSQL_ERROR_QUERY:            base = "Query error"; break;
    case MYSQL_ERROR_PROTOCOL:         base = "Protocol error"; break;
    case MYSQL_ERROR_TIMEOUT:          base = "Timeout"; break;
    case MYSQL_ERROR_SEND:             base = "Send error"; break;
    case MYSQL_ERROR_RECV:             base = "Receive error"; break;
    case MYSQL_ERROR_CONNECTION_CLOSED:base = "Connection closed"; break;
    case MYSQL_ERROR_PREPARED_STMT:    base = "Prepared statement error"; break;
    case MYSQL_ERROR_TRANSACTION:      base = "Transaction error"; break;
    case MYSQL_ERROR_SERVER:           base = "Server error"; break;
    case MYSQL_ERROR_INTERNAL:         base = "Internal error"; break;
    case MYSQL_ERROR_BUFFER_OVERFLOW:  base = "Buffer overflow"; break;
    case MYSQL_ERROR_INVALID_PARAM:    base = "Invalid parameter"; break;
    default:                           base = "unknown error"; break;
    }
    if (m_server_errno != 0) {
        base += " (errno=" + std::to_string(m_server_errno) + ")";
    }
    if (!m_extra_msg.empty()) {
        base += ": " + m_extra_msg;
    }
    return base;
}

} // namespace galay::mysql
