/**
 * @file tcp_socket.cc
 * @brief 异步 TCP 套接字操作实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details TcpSocket 生命周期（create、bind、listen、move、destroy）的具体实现。
 * 异步操作（accept、connect、send、recv、close）在头文件中内联，
 * 委托给调度器的可等待对象。
 */

#include "tcp_socket.h"
#include "kernel/common/defn.hpp"
#include <cerrno>
#include <unistd.h>

namespace galay::async
{

using namespace galay::kernel;

/**
 * @brief 为指定的 IP 协议版本构造 TCP 套接字
 *
 * @param type IP 协议类型（IPV4 或 IPV6）
 * @throws socket 创建失败时抛出 std::runtime_error
 */
TcpSocket::TcpSocket(IPType type)
    : m_controller(create(type))
{
    if(m_controller.m_handle == GHandle::invalid()) {
        throw std::runtime_error(strerror(errno));
    }
}

/**
 * @brief 将已有文件描述符包装为 TCP 套接字
 * @param handle 已有的套接字句柄（例如来自 accept）
 */
TcpSocket::TcpSocket(GHandle handle)
    : m_controller(handle)
{
}

/**
 * @brief 析构函数；不会自动关闭套接字
 */
TcpSocket::~TcpSocket()
{
}

/**
 * @brief 移动构造函数；转移 IO 控制器
 * @param other 被移动的对象
 */
TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : m_controller(std::move(other.m_controller))
{
}

/**
 * @brief 移动赋值运算符；关闭当前套接字后再转移
 * @param other 被移动的对象
 * @return 当前对象的引用
 */
TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
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
 * @brief 为给定地址族创建 TCP 套接字文件描述符
 *
 * @param type IP 协议类型（IPV4 映射到 AF_INET，IPV6 映射到 AF_INET6）
 * @return 成功时返回有效的 GHandle，失败时返回无效的 GHandle
 */
GHandle TcpSocket::create(IPType type)
{
    int domain = (type == IPType::IPV4) ? AF_INET : AF_INET6;
    int fd = socket(domain, SOCK_STREAM, 0);
    if (fd < 0) {
        return GHandle::invalid();
    }
    return {.fd = fd};
}

/**
 * @brief 将套接字绑定到本地地址
 *
 * @param host 要绑定的本地地址（IP 和端口）
 * @return 成功返回 void，失败返回带 kBindFailed 的 IOError
 */
std::expected<void, IOError> TcpSocket::bind(const Host& host)
{
    if (::bind(m_controller.m_handle.fd, host.sockAddr(), host.addrLen()) < 0) {
        return std::unexpected(IOError(kBindFailed, errno));
    }
    return {};
}

/**
 * @brief 开始监听传入连接
 *
 * @param backlog 待处理连接队列的最大长度（默认 128）
 * @return 成功返回 void，失败返回带 kListenFailed 的 IOError
 */
std::expected<void, IOError> TcpSocket::listen(int backlog)
{
    if (::listen(m_controller.m_handle.fd, backlog) < 0) {
        return std::unexpected(IOError(kListenFailed, errno));
    }
    return {};
}

}
