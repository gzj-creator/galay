#include "redis_error.h"
#include <array>
#include <utility>

namespace galay::redis
{
    static constexpr std::array<const char*, 21> msg = {{
        "success",
        "url invalid error",
        "host invalid error",
        "port invalid error",
        "db index invalid error",
        "address type invalid error",
        "version invalid error",
        "connection error",
        "free redis object error",
        "command error",
        "timeout error",
        "auth error",
        "invalid error",
        "unknown error",
        "parse error",
        "send error",
        "recv error",
        "buffer overflow error",
        "network error",
        "connection closed",
        "internal error",
    }};


    RedisError::RedisError(RedisErrorType type)
        : m_type(type)
    {
    }

    RedisError::RedisError(RedisErrorType type, std::string extra_msg)
        : m_extra_msg(std::move(extra_msg))
        , m_type(type)
    {
    }

    RedisError::RedisError(const galay::kernel::IOError& io_error)
        : m_extra_msg(io_error.message())
        , m_type(REDIS_ERROR_TYPE_NETWORK_ERROR)
    {
        using galay::kernel::IOError;
        using namespace galay::kernel;

        if (IOError::contains(io_error.code(), kTimeout)) {
            m_type = REDIS_ERROR_TYPE_TIMEOUT_ERROR;
            return;
        }
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_type = REDIS_ERROR_TYPE_CONNECTION_CLOSED;
            return;
        }
        if (IOError::contains(io_error.code(), kNotRunningOnIOScheduler) ||
            IOError::contains(io_error.code(), kNotReady)) {
            m_type = REDIS_ERROR_TYPE_INTERNAL_ERROR;
            return;
        }
        m_type = REDIS_ERROR_TYPE_CONNECTION_ERROR;
    }

    RedisErrorType RedisError::type() const
    {
        return m_type;
    }

    std::string RedisError::message() const
    {
        auto idx = static_cast<size_t>(m_type);
        const char* base_msg = (idx < msg.size()) ? msg[idx] : "unknown error";
        if(! m_extra_msg.empty()) {
            return std::string(base_msg) + " extra:" + m_extra_msg;
        }
        return base_msg;
    }
}
