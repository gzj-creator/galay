/**
 * @file handle_option.h
 * @brief 套接字句柄选项配置
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供套接字句柄的配置方法，包括：
 * - 阻塞/非阻塞模式
 * - 地址复用（SO_REUSEADDR）
 * - 端口复用（SO_REUSEPORT）
 * - IPv6-only / dual-stack 控制（IPV6_V6ONLY）
 * - 低延迟传输（TCP_NODELAY）
 * - 局部抑制 socket 写入触发 SIGPIPE（SO_NOSIGPIPE）
 *
 * @code
 * TcpSocket socket(scheduler);
 * socket.create();
 *
 * // 链式选项调用
 * socket.option().handleReuseAddr();
 * socket.option().handleReusePort();
 * socket.option().handleNonBlock();
 *
 * // 或检查返回值
 * auto result = socket.option().handleNonBlock();
 * if (!result) {
 *     return std::unexpected(result.error());
 * }
 * @endcode
 */

#ifndef GALAY_KERNEL_HANDLE_OPTION_H
#define GALAY_KERNEL_HANDLE_OPTION_H

#include "defn.hpp"
#include "error.h"
#include <expected>

namespace galay::kernel
{

/**
 * @brief 套接字句柄选项配置器
 *
 * @details 轻量级包装器，用于配置套接字句柄的选项。
 * 可按值传递和返回。
 *
 * @note
 * - 所有方法均为同步且立即生效
 * - 失败时返回包含系统错误码的 IOError
 * - 跨平台：Linux / macOS / Windows
 *
 * @see TcpSocket::option()
 */
class HandleOption
{
public:
    /**
     * @brief 从句柄构造
     * @param handle 要配置的套接字句柄
     * @note 不验证句柄；调用方必须确保句柄有效
     */
    explicit HandleOption(GHandle handle);

    /**
     * @brief 将句柄设置为阻塞模式
     *
     * @return std::expected<void, IOError> 成功返回 void，失败返回 IOError
     *
     * @details 在阻塞模式下，I/O 操作会阻塞直到完成或发生错误。
     * 这是套接字的默认模式。
     *
     * @note
     * - Linux/macOS：使用 fcntl 清除 O_NONBLOCK
     * - Windows：使用 ioctlsocket 设置 FIONBIO = 0
     */
    std::expected<void, IOError> handleBlock();

    /**
     * @brief 将句柄设置为非阻塞模式
     *
     * @return std::expected<void, IOError> 成功返回 void，失败返回 IOError
     *
     * @details 在非阻塞模式下，I/O 操作若无法完成则立即返回
     * EAGAIN/EWOULDBLOCK。异步 I/O 所必需。
     *
     * @note
     * - Linux/macOS：使用 fcntl 设置 O_NONBLOCK
     * - Windows：使用 ioctlsocket 设置 FIONBIO = 1
     *
     * @code
     * socket.option().handleNonBlock();  // 异步 I/O 前必须调用
     * @endcode
     */
    std::expected<void, IOError> handleNonBlock();

    /**
     * @brief 启用 SO_REUSEADDR 以允许重新绑定处于 TIME_WAIT 状态的端口
     *
     * @return std::expected<void, IOError> 成功返回 void，失败返回 IOError
     *
     * @details 服务器通常需要此选项以在重启时不遇到"地址已被使用"错误。
     *
     * @note 在 bind() 之前调用
     *
     * @code
     * socket.option().handleReuseAddr();
     * socket.bind(host);
     * @endcode
     */
    std::expected<void, IOError> handleReuseAddr();

    /**
     * @brief 启用 SO_REUSEPORT 用于多进程/多线程负载均衡
     *
     * @return std::expected<void, IOError> 成功返回 void，失败返回 IOError
     *
     * @details 允许多个套接字绑定到同一端口。
     *
     * @note
     * - Linux 3.9+ 和 macOS 支持
     * - Windows 上回退到 SO_REUSEADDR
     * - 绑定到同一端口的所有套接字都必须设置此选项
     *
     * @code
     * socket.option().handleReusePort();
     * socket.bind(Host(IPType::IPV4, "0.0.0.0", 8080));
     * @endcode
     */
    std::expected<void, IOError> handleReusePort();

    /**
     * @brief 设置 IPv6 socket 的 IPV6_V6ONLY 选项
     *
     * @param enabled true 表示仅接受 IPv6；false 表示允许系统支持的 IPv4-mapped IPv6 连接。
     * @return std::expected<void, IOError> 成功返回 void，失败返回 IOError
     *
     * @details 该选项控制绑定到 IPv6 wildcard 地址（例如 "::"）的监听 socket 是否可同时接受 IPv4。
     * 具体 dual-stack 行为仍受操作系统支持和全局配置影响。
     *
     * @note
     * - 必须在 bind() 之前调用
     * - 仅适用于 AF_INET6 socket；用于 IPv4 socket 会由系统调用返回错误
     */
    std::expected<void, IOError> handleIPv6Only(bool enabled = true);

    /**
     * @brief 通过 TCP_NODELAY 禁用 Nagle 算法
     *
     * @return std::expected<void, IOError> 成功返回 void，失败返回 IOError
     *
     * @details 通过禁用 Nagle 缓冲延迟来减少小写入的延迟。
     * 推荐用于延迟敏感的请求/响应和 WebSocket 场景。
     *
     * @note 在连接建立后尽快调用
     */
    std::expected<void, IOError> handleTcpNoDelay();

    /**
     * @brief 在支持的平台上启用 SO_NOSIGPIPE
     *
     * @return std::expected<void, IOError> 成功返回 void，失败返回 IOError
     *
     * @details
     * - macOS/BSD：设置 SO_NOSIGPIPE，使该 socket 写入断开连接时返回 EPIPE 而不是投递 SIGPIPE
     * - 其他平台：静默成功，不修改进程级 signal disposition
     *
     * @note 这是 socket 局部策略，框架不会修改全局 SIGPIPE 处理方式。
     */
    std::expected<void, IOError> handleNoSigPipe();

    /**
     * @brief 启用 TCP_DEFER_ACCEPT（仅 Linux）
     *
     * @param seconds 在唤醒 accept 之前等待第一个数据包的最大时间
     * @return std::expected<void, IOError> 成功返回 void，失败返回 IOError
     *
     * @details
     * - Linux：设置 TCP_DEFER_ACCEPT，使 accept 仅在数据到达时唤醒
     * - 非 Linux：静默成功，不改变行为
     *
     * @note
     * - 仅在监听套接字上调用，且在 listen() 之前
     * - seconds 必须为正数；值过大会增加首包延迟
     */
    std::expected<void, IOError> handleTcpDeferAccept(int seconds = 1);

private:
    GHandle m_handle;  ///< 要配置的套接字句柄
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_HANDLE_OPTION_H
