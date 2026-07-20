#include "etcd_error.h"

#include <array>

namespace galay::etcd
{

namespace
{
constexpr std::array<const char*, 13> kErrorMessages = {
    "success",
    "invalid endpoint",
    "invalid parameter",
    "not connected",
    "connection error",
    "timeout",
    "send error",
    "recv error",
    "http error",
    "server error",
    "parse error",
    "client pool exhausted",
    "internal error",
};
}

EtcdError::EtcdError(EtcdErrorType type)
    : m_type(type)
{
}

EtcdError::EtcdError(EtcdErrorType type, std::string extra_msg)
    : m_extra_msg(std::move(extra_msg))
    , m_type(type)
{
}

EtcdErrorType EtcdError::type() const
{
    return m_type;
}

std::string EtcdError::message() const
{
    const size_t index = static_cast<size_t>(m_type);
    std::string base = index < kErrorMessages.size() ? kErrorMessages[index] : "unknown error";
    if (!m_extra_msg.empty()) {
        base += ": ";
        base += m_extra_msg;
    }
    return base;
}

bool EtcdError::isOk() const
{
    return m_type == EtcdErrorType::Success;
}

} // namespace galay::etcd
