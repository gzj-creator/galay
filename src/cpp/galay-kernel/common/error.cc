/**
 * @file error.cc
 * @brief IOError 和错误工具函数的实现
 * @author galay-kernel
 * @version 1.0.0
 */

#include "error.h"
#include <cstdint>
#include <sstream>
#include <cstring>

namespace galay::kernel
{
/**
 * @brief 按 IOErrorCode 索引的人类可读描述
 */
const char* error_string[] = {
    "Connection disconnected",
    "Event notified but source not ready",
    "Parameter invalid",
    "Failed to receive data",
    "Failed to send data",
    "Failed to accept connection",
    "Failed to connect to remote host",
    "Failed to bind socket address",
    "Failed to listen on socket",
    "Failed to open file",
    "Failed to read file",
    "Failed to write file",
    "Failed to get file status",
    "Failed to sync file",
    "Failed to seek file",
    "Operation timeout",
    "Not running on IO scheduler"
};

/**
 * @brief 检查打包的 64 位错误码是否包含指定的 IOErrorCode
 * @param error 打包的错误值（高 32 位：系统错误码，低 32 位：I/O 错误码）
 * @param code 要测试的 IOErrorCode
 * @return 若低 32 位与请求的码匹配则返回 true
 */
bool IOError::contains(uint64_t error, IOErrorCode code)
{
    uint32_t io_error_code = error & 0xffffffff;
    return static_cast<uint32_t>(code) == io_error_code;
}

/**
 * @brief 从 IOErrorCode 和系统错误码构造 IOError
 * @param io_error_code 内核级错误类别
 * @param system_code 平台错误（POSIX 为 errno，Windows 为 WSAGetLastError）
 */
IOError::IOError(IOErrorCode io_error_code, uint32_t system_code)
    : m_code(makeErrorCode(io_error_code, system_code))
{
}

/**
 * @brief 获取打包的 64 位错误码
 * @return 错误码（高 32 位：系统错误，低 32 位：I/O 错误）
 */
uint64_t IOError::code() const
{
    return m_code;
}

/**
 * @brief 构建人类可读的错误消息
 * @return 结合 I/O 错误描述和系统 strerror 的字符串
 */
std::string IOError::message() const
{
    uint32_t io_error_code = m_code & 0xffffffff;
    uint32_t system_code = m_code >> 32;
    std::stringstream str;
    str << error_string[io_error_code];
    if(system_code != 0) {
        str << " (sys: " << strerror(system_code) << ")";
    } else {
        str << " (sys: no error)";
    }
    return str.str();
}

/**
 * @brief 将错误重置为无错误状态（code = 0）
 */
void IOError::reset()
{
    m_code = 0;
}

/**
 * @brief 将 IOErrorCode 和系统错误码打包为单个 64 位值
 * @param io_error_code 低 32 位
 * @param system_code 高 32 位
 * @return 打包的 64 位错误码
 */
uint64_t IOError::makeErrorCode(IOErrorCode io_error_code, uint32_t system_code)
{
    uint64_t ret = system_code;
    ret = ret << 32;
    return ret | static_cast<uint32_t>(io_error_code);
}
}
