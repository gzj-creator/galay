/**
 * @file udp_socket.cc
 * @brief 异步 UDP 套接字操作实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details UdpSocket 生命周期（create、bind、move、destroy）的具体实现。
 * 异步操作（recvfrom、sendto、close）在此定义，委托给调度器的可等待对象。
 */

#include "udp_socket.h"
#include <galay/cpp/galay-kernel/common/defn.hpp>
#include <galay/cpp/galay-kernel/common/host.hpp>
#include <cerrno>
#include <unistd.h>

namespace galay::async
{

using namespace galay::kernel;

/**
 * @brief 为指定的 IP 协议版本构造 UDP 套接字
 *
 * @param type IP 协议类型（IPV4 或 IPV6）
 * @note 兼容旧调用；创建失败时内部句柄保持 invalid，错误原因通过 create() 返回。
 */
UdpSocket::UdpSocket(IPType type)
    : m_controller([](IPType socket_type) {
        auto opened = openHandle(socket_type);
        return opened ? *opened : GHandle::invalid();
    }(type))
{
}

/**
 * @brief 创建 UDP 套接字并返回显式错误。
 * @param type IP 协议类型（IPV4 或 IPV6）
 * @return 成功返回 UdpSocket；失败返回 IOError(kOpenFailed, errno)
 */
std::expected<UdpSocket, IOError> UdpSocket::create(IPType type)
{
    auto handle = openHandle(type);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    return UdpSocket(*handle);
}

/**
 * @brief 将已有文件描述符包装为 UDP 套接字
 * @param handle 已有的套接字句柄
 */
UdpSocket::UdpSocket(GHandle handle)
    : m_controller(handle)
{
}

/**
 * @brief 析构函数；不会自动关闭套接字
 */
UdpSocket::~UdpSocket()
{
}

/**
 * @brief 移动构造函数；转移 IO 控制器
 * @param other 被移动的对象
 */
UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : m_controller(std::move(other.m_controller))
{
}

/**
 * @brief 移动赋值运算符；关闭当前套接字后再转移
 * @param other 被移动的对象
 * @return 当前对象的引用
 */
UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other) {
        if (m_controller.m_handle != GHandle::invalid()) {
            ::close(m_controller.m_handle.fd);
        }
        m_controller = std::move(other.m_controller);
    }
    return *this;
}


/**
 * @brief 为给定地址族创建 UDP 套接字文件描述符
 *
 * @param type IP 协议类型（IPV4 映射到 AF_INET，IPV6 映射到 AF_INET6）
 * @return 成功时返回有效的 GHandle，失败时返回无效的 GHandle
 */
std::expected<GHandle, IOError> UdpSocket::openHandle(IPType type)
{
    int domain = (type == IPType::IPV4) ? AF_INET : AF_INET6;
    int fd = ::socket(domain, SOCK_DGRAM, 0);  // SOCK_DGRAM 用于 UDP
    if (fd < 0) {
        return std::unexpected(IOError(kOpenFailed, errno));
    }
    if (type == IPType::IPV6) {
        auto dual_stack = HandleOption(GHandle{.fd = fd}).handleIPv6Only(false);
        if (!dual_stack) {
            ::close(fd);
            return std::unexpected(dual_stack.error());
        }
    }
    return GHandle{.fd = fd};
}

/**
 * @brief 将套接字绑定到本地地址
 *
 * @param host 要绑定的本地地址（IP 和端口）
 * @return 成功返回 void，失败返回带 kBindFailed 的 IOError
 */
std::expected<void, IOError> UdpSocket::bind(const Host& host)
{
    if (::bind(m_controller.m_handle.fd, host.sockAddr(), host.addrLen()) < 0) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
    return {};
}

/**
 * @brief 创建用于异步数据报接收的 RecvFromAwaitable
 * @param buffer 接收数据的目标缓冲区
 * @param length 缓冲区大小（字节）
 * @param from 发送方地址的输出参数（可为 nullptr）
 * @return 绑定到该套接字 IO 控制器的 RecvFromAwaitable
 */
RecvFromAwaitable UdpSocket::recvfrom(char* buffer, size_t length, Host* from)
{
    return RecvFromAwaitable(&m_controller, buffer, length, from);
}

/**
 * @brief 创建用于异步数据报发送的 SendToAwaitable
 * @param buffer 要发送的源数据
 * @param length 要发送的字节数
 * @param to 目标地址
 * @return 绑定到该套接字 IO 控制器的 SendToAwaitable
 */
SendToAwaitable UdpSocket::sendto(const char* buffer, size_t length, const Host& to)
{
    return SendToAwaitable(&m_controller, buffer, length, to);
}

/**
 * @brief 创建用于异步套接字关闭的 CloseAwaitable
 * @return 绑定到该套接字 IO 控制器的 CloseAwaitable
 */
CloseAwaitable UdpSocket::close()
{
    return CloseAwaitable(&m_controller);
}

}
