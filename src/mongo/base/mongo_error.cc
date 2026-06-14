#include "mongo_error.h"

namespace galay::mongo
{

MongoError::MongoError(MongoErrorType type)
    : m_type(type)
{
}

MongoError::MongoError(MongoErrorType type, std::string extra_msg)
    : m_type(type)
    , m_extra_msg(std::move(extra_msg))
{
}

MongoError::MongoError(MongoErrorType type, int32_t server_code, std::string server_msg)
    : m_type(type)
    , m_server_code(server_code)
    , m_extra_msg(std::move(server_msg))
{
}

MongoErrorType MongoError::type() const
{
    return m_type;
}

int32_t MongoError::serverCode() const
{
    return m_server_code;
}

std::string MongoError::message() const
{
    std::string base;
    switch (m_type) {
    case MONGO_ERROR_SUCCESS:           base = "Success"; break;
    case MONGO_ERROR_CONNECTION:        base = "Connection error"; break;
    case MONGO_ERROR_AUTH:              base = "Authentication error"; break;
    case MONGO_ERROR_COMMAND:           base = "Command error"; break;
    case MONGO_ERROR_PROTOCOL:          base = "Protocol error"; break;
    case MONGO_ERROR_TIMEOUT:           base = "Timeout"; break;
    case MONGO_ERROR_SEND:              base = "Send error"; break;
    case MONGO_ERROR_RECV:              base = "Receive error"; break;
    case MONGO_ERROR_CONNECTION_CLOSED: base = "Connection closed"; break;
    case MONGO_ERROR_SERVER:            base = "Server error"; break;
    case MONGO_ERROR_INTERNAL:          base = "Internal error"; break;
    case MONGO_ERROR_BUFFER_OVERFLOW:   base = "Buffer overflow"; break;
    case MONGO_ERROR_INVALID_PARAM:     base = "Invalid parameter"; break;
    case MONGO_ERROR_UNSUPPORTED:       base = "Unsupported feature"; break;
    default:                            base = "Unknown error"; break;
    }

    if (m_server_code != 0) {
        base += " (code=" + std::to_string(m_server_code) + ")";
    }

    if (!m_extra_msg.empty()) {
        base += ": " + m_extra_msg;
    }

    return base;
}

} // namespace galay::mongo
