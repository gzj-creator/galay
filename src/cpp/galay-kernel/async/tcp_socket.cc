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
#include <galay/cpp/galay-kernel/common/defn.hpp>
#include <unistd.h>
#include <cerrno>

namespace galay::async
{

using namespace galay::kernel;

/**
 * @brief 为指定的 IP 协议版本构造 TCP 套接字
 *
 * @param type IP 协议类型（IPV4 或 IPV6）
 * @note 兼容旧调用；创建失败时内部句柄保持 invalid，错误原因通过 create() 返回。
 */
TcpSocket::TcpSocket(IPType type)
    : m_controller([](IPType socket_type) {
        auto opened = openHandle(socket_type);
        return opened ? *opened : GHandle::invalid();
    }(type))
{
}

/**
 * @brief 创建 TCP 套接字并返回显式错误。
 * @param type IP 协议类型（IPV4 或 IPV6）
 * @return 成功返回 TcpSocket；失败返回 IOError(kOpenFailed, errno)
 */
std::expected<TcpSocket, IOError> TcpSocket::create(IPType type)
{
    auto handle = openHandle(type);
    if (!handle) {
        return std::unexpected(handle.error());
    }
    return TcpSocket(*handle);
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
 * @brief 析构函数；关闭仍由对象持有的套接字
 */
TcpSocket::~TcpSocket()
{
    if (m_controller.m_handle != GHandle::invalid()) {
        galay_close(m_controller.m_handle.fd);
        m_controller.m_handle = GHandle::invalid();
    }
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
            galay_close(m_controller.m_handle.fd);
            m_controller.m_handle = GHandle::invalid();
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
std::expected<GHandle, IOError> TcpSocket::openHandle(IPType type)
{
    int domain = (type == IPType::IPV4) ? AF_INET : AF_INET6;
    int fd = socket(domain, SOCK_STREAM, 0);
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
 * @return 成功返回 void；Host 非法返回 kParamInvalid；系统 bind 失败返回 kBindFailed
 */
std::expected<void, IOError> TcpSocket::bind(const Host& host)
{
    if (!host.valid()) {
        return std::unexpected(IOError(kParamInvalid, 0));
    }
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
