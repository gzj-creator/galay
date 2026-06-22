/**
 * @file handle_option.cc
 * @brief 套接字句柄选项的平台特定实现
 * @author galay-kernel
 * @version 1.0.0
 */

#include "handle_option.h"
#include <fcntl.h>
#include <cerrno>

#if defined(__linux__) || defined(__APPLE__)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace galay::kernel
{

/**
 * @brief 存储要配置的句柄
 * @param handle 平台相关的套接字/文件描述符
 */
HandleOption::HandleOption(GHandle handle)
    : m_handle(handle)
{
}

/**
 * @brief 将句柄设置为阻塞模式
 * @details POSIX 上使用 fcntl 清除 O_NONBLOCK，Windows 上使用 ioctlsocket
 * @return 成功返回 void，失败返回 IOError
 */
std::expected<void, IOError> HandleOption::handleBlock()
{
#if defined(_WIN32) || defined(_WIN64)
    u_long mode = 0;
    if (ioctlsocket(m_handle.fd, FIONBIO, &mode) != 0) {
        return std::unexpected(IOError(kBindFailed, WSAGetLastError()));
    }
#else
    int flags = fcntl(m_handle.fd, F_GETFL, 0);
    if (flags == -1) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
    if (fcntl(m_handle.fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
#endif
    return {};
}

/**
 * @brief 将句柄设置为非阻塞模式
 * @details POSIX 上使用 fcntl 设置 O_NONBLOCK，Windows 上使用 ioctlsocket
 * @return 成功返回 void，失败返回 IOError
 */
std::expected<void, IOError> HandleOption::handleNonBlock()
{
#if defined(_WIN32) || defined(_WIN64)
    u_long mode = 1;
    if (ioctlsocket(m_handle.fd, FIONBIO, &mode) != 0) {
        return std::unexpected(IOError(kBindFailed, WSAGetLastError()));
    }
#else
    int flags = fcntl(m_handle.fd, F_GETFL, 0);
    if (flags == -1) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
    if (fcntl(m_handle.fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
#endif
    return {};
}

/**
 * @brief 启用 SO_REUSEADDR 以允许在 TIME_WAIT 期间重新绑定
 * @return 成功返回 void，失败返回 IOError
 */
std::expected<void, IOError> HandleOption::handleReuseAddr()
{
    int opt = 1;
#if defined(_WIN32) || defined(_WIN64)
    if (setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0) {
        return std::unexpected(IOError(kBindFailed, WSAGetLastError()));
    }
#else
    if (setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
#endif
    return {};
}

/**
 * @brief 启用 SO_REUSEPORT 用于多进程负载均衡
 * @details Windows 上回退到 SO_REUSEADDR
 * @return 成功返回 void，失败返回 IOError
 */
std::expected<void, IOError> HandleOption::handleReusePort()
{
#if defined(_WIN32) || defined(_WIN64)
    // Windows 没有 SO_REUSEPORT，改用 SO_REUSEADDR
    return handleReuseAddr();
#else
    int opt = 1;
    if (setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) != 0) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
    return {};
#endif
}

/**
 * @brief 设置 IPv6 socket 的 IPV6_V6ONLY 选项
 * @param enabled true 表示仅 IPv6；false 表示允许系统支持的 dual-stack
 * @return 成功返回 void，失败返回 IOError
 */
std::expected<void, IOError> HandleOption::handleIPv6Only(bool enabled)
{
    if (m_handle.fd < 0) {
        return std::unexpected(IOError(kParamInvalid, 0));
    }

#if !defined(IPV6_V6ONLY)
    (void)enabled;
    return std::unexpected(IOError(kParamInvalid, 0));
#else
    int opt = enabled ? 1 : 0;
#if defined(_WIN32) || defined(_WIN64)
    if (::setsockopt(m_handle.fd, IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0) {
        return std::unexpected(IOError(kBindFailed, WSAGetLastError()));
    }
#else
    if (::setsockopt(m_handle.fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) != 0) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
#endif
    return {};
#endif
}

/**
 * @brief 通过 TCP_NODELAY 禁用 Nagle 算法
 * @return 成功返回 void，句柄无效时返回 IOError（kParamInvalid）
 */
std::expected<void, IOError> HandleOption::handleTcpNoDelay()
{
    if (m_handle.fd < 0) {
        return std::unexpected(IOError(kParamInvalid, 0));
    }

    int opt = 1;
#if defined(_WIN32) || defined(_WIN64)
    if (::setsockopt(m_handle.fd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0) {
        return std::unexpected(IOError(kBindFailed, WSAGetLastError()));
    }
#else
    if (::setsockopt(m_handle.fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) != 0) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
#endif

    return {};
}

/**
 * @brief 在 Linux 上启用 TCP_DEFER_ACCEPT；其他平台为空操作
 * @param seconds accept 返回前等待第一个数据包的最大时间
 * @return 成功返回 void，句柄无效或 seconds <= 0 时返回 IOError
 */
std::expected<void, IOError> HandleOption::handleTcpDeferAccept(int seconds)
{
    if (m_handle.fd < 0) {
        return std::unexpected(IOError(kParamInvalid, 0));
    }
    if (seconds <= 0) {
        return std::unexpected(IOError(kParamInvalid, 0));
    }

#if defined(__linux__)
    int opt = seconds;
    if (::setsockopt(m_handle.fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &opt, sizeof(opt)) != 0) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
#else
    (void)seconds;
#endif

    return {};
}

}
