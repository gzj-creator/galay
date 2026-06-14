/**
 * @file tcp_socket.h
 * @brief 异步TCP Socket封装
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供基于协程的异步TCP Socket操作，包括：
 * - 服务端：create -> bind -> listen -> accept
 * - 客户端：create -> connect -> send/recv
 *
 * @example
 * @code
 * // 服务端示例
 * Task<void> server() {
 *     TcpSocket listener;
 *     listener.option().handleReuseAddr();
 *     listener.option().handleNonBlock();
 *     listener.bind(Host(IPType::IPV4, "0.0.0.0", 8080));
 *     listener.listen(1024);
 *
 *     while (true) {
 *         Host clientHost;
 *         auto result = co_await listener.accept(&clientHost);
 *         if (result) {
 *             // 处理新连接
 *         }
 *     }
 * }
 *
 * // 客户端示例
 * Task<void> client() {
 *     TcpSocket socket;
 *     socket.option().handleNonBlock();
 *
 *     co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", 8080));
 *     co_await socket.send("Hello", 5);
 *
 *     char buffer[1024];
 *     auto result = co_await socket.recv(buffer, sizeof(buffer));
 *
 *     co_await socket.close();
 * }
 * @endcode
 */

#ifndef GALAY_KERNEL_ASYNC_TCP_SOCKET_H
#define GALAY_KERNEL_ASYNC_TCP_SOCKET_H

#include "kernel/common/defn.hpp"
#include "kernel/common/error.h"
#include "kernel/common/host.hpp"
#include "kernel/common/handle_option.h"
#include "kernel/kernel/io_scheduler.hpp"
#include "kernel/kernel/awaitable.h"
#include <array>
#include <expected>

namespace galay::async
{
/**
 * @brief 异步TCP Socket类
 *
 * @details 封装底层 socket 操作，提供 Task 友好的异步 IO 接口。
 * 内部包含：
 * - GHandle: 底层socket句柄
 * - IOScheduler*: IO调度器指针
 * - IOController: IO事件控制器
 *
 * @note
 * - 不可拷贝，仅支持移动语义
 * - 析构时不会自动关闭socket，需显式调用close()
 * - 所有异步操作需要在任务中使用 co_await
 *
 * @see IOScheduler, HandleOption, Host
 */
class TcpSocket
{
public:
    /**
     * @brief 构造函数，创建Socket
     * @param type IP协议类型（IPV4/IPV6）
     * @note 创建失败会抛出异常
     */
    explicit TcpSocket(galay::kernel::IPType type = galay::kernel::IPType::IPV4);

    /**
     * @brief 从已有句柄构造Socket
     * @param handle 已有的socket句柄（如accept返回的句柄）
     * @note 用于包装已存在的socket，如服务端accept得到的客户端连接
     */
    explicit TcpSocket(GHandle handle);

    /**
     * @brief 析构函数
     * @note 不会自动关闭socket，需显式调用close()
     */
    ~TcpSocket();

    /// @brief 禁用拷贝构造
    TcpSocket(const TcpSocket&) = delete;
    /// @brief 禁用拷贝赋值
    TcpSocket& operator=(const TcpSocket&) = delete;

    /**
     * @brief 移动构造函数
     * @param other 被移动的对象，移动后other变为无效状态
     */
    TcpSocket(TcpSocket&& other) noexcept;

    /**
     * @brief 移动赋值运算符
     * @param other 被移动的对象
     * @return 当前对象的引用
     */
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    /**
     * @brief 获取底层socket句柄
     * @return GHandle 底层句柄，可用于底层操作
     */
    GHandle handle() const { return m_controller.m_handle; }

    /**
     * @brief 获取IO控制器指针
     * @return IOController* 内部IO控制器，用于高级操作
     */
    galay::kernel::IOController* controller() { return &m_controller; }


    /**
     * @brief 绑定本地地址
     *
     * @param host 要绑定的地址（IP和端口）
     * @return std::expected<void, IOError> 成功返回void，失败返回IOError
     *
     * @note 服务端必须调用，客户端通常不需要
     *
     * @code
     * socket.bind(Host(IPType::IPV4, "0.0.0.0", 8080));
     * @endcode
     */
    std::expected<void, galay::kernel::IOError> bind(const galay::kernel::Host& host);  ///< 绑定本地地址；成功返回 void，失败返回 IOError

    /**
     * @brief 开始监听连接
     *
     * @param backlog 等待连接队列的最大长度，默认128
     * @return std::expected<void, IOError> 成功返回void，失败返回IOError
     *
     * @note 仅服务端需要，必须在bind之后调用
     */
    std::expected<void, galay::kernel::IOError> listen(int backlog = 128);

    /**
     * @brief 获取句柄选项配置器
     *
     * @return HandleOption 选项配置器对象
     *
     * @code
     * socket.option().handleReuseAddr();  // 设置地址重用
     * socket.option().handleNonBlock();   // 设置非阻塞
     * @endcode
     */
    galay::kernel::HandleOption option() { return galay::kernel::HandleOption(m_controller.m_handle); }

    /**
     * @brief 异步接受新连接
     *
     * @param clientHost 输出参数，接收客户端地址信息，可为nullptr
     * @return AcceptAwaitable 可等待对象，co_await后返回新连接的句柄
     *
     * @note
     * - 仅服务端使用
     * - 必须在listen之后调用
     * - 返回的GHandle需要用新的TcpSocket包装
     *
     * @code
     * Host clientHost;
     * auto result = co_await listener.accept(&clientHost);
     * if (result) {
     *     TcpSocket client(scheduler, result.value());
     *     // 处理客户端连接
     * }
     * @endcode
     */
    galay::kernel::AcceptAwaitable accept(galay::kernel::Host* clientHost) {
        return galay::kernel::AcceptAwaitable(&m_controller, clientHost);
    }

    /**
     * @brief 异步连接到服务器
     *
     * @param host 目标服务器地址
     * @return ConnectAwaitable 可等待对象，co_await后返回连接结果
     *
     * @note
     * - 仅客户端使用
     * - 必须在create之后调用
     * - 建议先设置非阻塞模式
     *
     * @code
     * auto result = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", 8080));
     * if (!result) {
     *     // 连接失败
     * }
     * @endcode
     */
    galay::kernel::ConnectAwaitable connect(const galay::kernel::Host& host) {
        return galay::kernel::ConnectAwaitable(&m_controller, host);
    }

    /**
     * @brief 异步接收数据
     *
     * @param buffer 接收缓冲区指针
     * @param length 缓冲区大小
     * @return RecvAwaitable 可等待对象，co_await后返回接收到的字节数
     *
     * @note
     * - 返回值为0表示对端关闭连接
     * - 缓冲区生命周期必须持续到co_await完成
     *
     * @code
     * char buffer[1024];
     * auto result = co_await socket.recv(buffer, sizeof(buffer));
     * if (result) {
     *     size_t bytes = result.value();
     *     if (bytes == 0) {
     *         // 对端关闭
     *     } else {
     *         // 处理数据
     *     }
     * }
     * @endcode
     */
    galay::kernel::RecvAwaitable recv(char* buffer, size_t length) {
        return galay::kernel::RecvAwaitable(&m_controller, buffer, length);
    }

    /**
     * @brief 异步发送数据
     *
     * @param buffer 发送数据指针
     * @param length 数据长度
     * @return SendAwaitable 可等待对象，co_await后返回实际发送的字节数
     *
     * @note
     * - 返回值可能小于length，表示部分发送
     * - 缓冲区生命周期必须持续到co_await完成
     *
     * @code
     * const char* msg = "Hello";
     * auto result = co_await socket.send(msg, strlen(msg));
     * if (result) {
     *     size_t sent = result.value();
     * }
     * @endcode
     */
    galay::kernel::SendAwaitable send(const char* buffer, size_t length) {
        return galay::kernel::SendAwaitable(&m_controller, buffer, length);
    }

    /**
     * @brief 异步 scatter-gather 读取数据（span 动态视图）
     *
     * @param iovecs iovec 视图，协程挂起期间底层存储由调用方持有
     * @return ReadvAwaitable 可等待对象，co_await 后返回读取的总字节数
     *
     * @note
     * - 适合使用 vector/span 组织的动态批量缓冲区
     * - 底层 iovec 存储生命周期必须持续到 co_await 完成
     */
    galay::kernel::ReadvAwaitable readv(std::span<const struct iovec> iovecs) {
        return galay::kernel::ReadvAwaitable(&m_controller, iovecs);
    }

    /**
     * @brief 异步 scatter-gather 读取数据（数组借用快路径）
     *
     * @param iovecs iovec 数组左值，协程挂起期间由调用方持有
     * @param count 实际使用的 iovec 数量，必须满足 count <= N
     * @return ReadvAwaitable 可等待对象，co_await 后返回读取的总字节数
     *
     * @note
     * - 该重载不会复制 iovec 元数据，适合热路径
     * - iovecs 必须是左值数组，生命周期必须持续到 co_await 完成
     * - count > N 会直接终止进程，而不是静默截断
     */
    template<size_t N>
    galay::kernel::ReadvAwaitable readv(std::array<struct iovec, N>& iovecs, size_t count = N) {
        return galay::kernel::ReadvAwaitable(&m_controller, iovecs, count);
    }

    /**
     * @brief 异步 scatter-gather 读取数据（C 数组借用快路径）
     *
     * @param iovecs iovec C 数组左值，协程挂起期间由调用方持有
     * @param count 实际使用的 iovec 数量，必须满足 count <= N
     * @return ReadvAwaitable 可等待对象，co_await 后返回读取的总字节数
     *
     * @note
     * - 该重载不会复制 iovec 元数据，适合热路径
     * - iovecs 必须是左值数组，生命周期必须持续到 co_await 完成
     * - count > N 会直接终止进程，而不是静默截断
     */
    template<size_t N>
    galay::kernel::ReadvAwaitable readv(struct iovec (&iovecs)[N], size_t count = N) {
        return galay::kernel::ReadvAwaitable(&m_controller, iovecs, count);
    }

    /**
     * @brief 异步 scatter-gather 写入数据（span 动态视图）
     *
     * @param iovecs iovec 视图，协程挂起期间底层存储由调用方持有
     * @return WritevAwaitable 可等待对象，co_await 后返回写入的总字节数
     *
     * @note
     * - 适合使用 vector/span 组织的动态批量缓冲区
     * - 底层 iovec 存储生命周期必须持续到 co_await 完成
     */
    galay::kernel::WritevAwaitable writev(std::span<const struct iovec> iovecs) {
        return galay::kernel::WritevAwaitable(&m_controller, iovecs);
    }

    /**
     * @brief 异步 scatter-gather 写入数据（数组借用快路径）
     *
     * @param iovecs iovec 数组左值，协程挂起期间由调用方持有
     * @param count 实际使用的 iovec 数量，必须满足 count <= N
     * @return WritevAwaitable 可等待对象，co_await 后返回写入的总字节数
     *
     * @note
     * - 该重载不会复制 iovec 元数据，适合热路径
     * - iovecs 必须是左值数组，生命周期必须持续到 co_await 完成
     * - count > N 会直接终止进程，而不是静默截断
     */
    template<size_t N>
    galay::kernel::WritevAwaitable writev(std::array<struct iovec, N>& iovecs, size_t count = N) {
        return galay::kernel::WritevAwaitable(&m_controller, iovecs, count);
    }

    /**
     * @brief 异步 scatter-gather 写入数据（C 数组借用快路径）
     *
     * @param iovecs iovec C 数组左值，协程挂起期间由调用方持有
     * @param count 实际使用的 iovec 数量，必须满足 count <= N
     * @return WritevAwaitable 可等待对象，co_await 后返回写入的总字节数
     *
     * @note
     * - 该重载不会复制 iovec 元数据，适合热路径
     * - iovecs 必须是左值数组，生命周期必须持续到 co_await 完成
     * - count > N 会直接终止进程，而不是静默截断
     */
    template<size_t N>
    galay::kernel::WritevAwaitable writev(struct iovec (&iovecs)[N], size_t count = N) {
        return galay::kernel::WritevAwaitable(&m_controller, iovecs, count);
    }

    /**
     * @brief 异步零拷贝发送文件
     *
     * @param file_fd 要发送的文件描述符
     * @param offset 文件偏移量（发送起始位置）
     * @param count 要发送的字节数
     * @return SendFileAwaitable 可等待对象，co_await后返回实际发送的字节数
     *
     * @note
     * - 使用 sendfile 系统调用实现零拷贝传输，性能优于 read + send
     * - 适用于发送大文件的场景
     * - 返回值可能小于 count，表示部分发送
     * - 不同平台的 sendfile 接口略有差异，本方法已做跨平台适配
     * - 文件描述符需要由调用者管理（打开和关闭）
     *
     * @code
     * int file_fd = open("large_file.dat", O_RDONLY);
     * if (file_fd >= 0) {
     *     auto result = co_await socket.sendfile(file_fd, 0, 1024*1024);
     *     if (result) {
     *         size_t sent = result.value();
     *         // 处理发送结果
     *     }
     *     close(file_fd);
     * }
     * @endcode
     */
    galay::kernel::SendFileAwaitable sendfile(int file_fd, off_t offset, size_t count) {
        return galay::kernel::SendFileAwaitable(&m_controller, file_fd, offset, count);
    }

    /**
     * @brief 异步关闭socket
     *
     * @return CloseAwaitable 可等待对象，co_await后返回关闭结果
     *
     * @note 关闭后socket变为无效状态，不可再使用
     *
     * @code
     * co_await socket.close();
     * @endcode
     */
    galay::kernel::CloseAwaitable close() {
        return galay::kernel::CloseAwaitable(&m_controller);
    }

    /*
     * @brief 获取IO控制器
     * @return IOController* IO控制器
     */
    galay::kernel::IOController* getController() { return &m_controller; }

private:
    GHandle create(galay::kernel::IPType type);  ///< 按协议版本创建底层 socket；失败时返回无效句柄
private:
    galay::kernel::IOController m_controller;  ///< IO事件控制器
};

} // namespace galay::async

#endif // GALAY_KERNEL_ASYNC_TCP_SOCKET_H
