/**
 * @file async_udp.h
 * @brief 异步UDP Socket封装
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供基于协程的异步UDP Socket操作，包括：
 * - 服务端：create -> bind -> recvfrom/sendto
 * - 客户端：create -> sendto/recvfrom
 *
 * @example
 * @code
 * // 服务端示例
 * Task<void> server() {
 *     AsyncUdpSocket socket;
 *     socket.option().handleReuseAddr();
 *     socket.option().handleNonBlock();
 *     socket.bind(Host(IPType::IPV4, "0.0.0.0", 8080));
 *
 *     while (true) {
 *         Host clientHost;
 *         char buffer[1024];
 *         auto result = co_await socket.recvfrom(buffer, sizeof(buffer), &clientHost);
 *         if (result) {
 *             // 处理接收到的数据
 *             co_await socket.sendto(buffer, result.value().size(), clientHost);
 *         }
 *     }
 * }
 *
 * // 客户端示例
 * Task<void> client() {
 *     AsyncUdpSocket socket;
 *     socket.option().handleNonBlock();
 *
 *     Host server(IPType::IPV4, "127.0.0.1", 8080);
 *     co_await socket.sendto("Hello", 5, server);
 *
 *     char buffer[1024];
 *     Host from;
 *     auto result = co_await socket.recvfrom(buffer, sizeof(buffer), &from);
 *
 *     co_await socket.close();
 * }
 * @endcode
 */

#ifndef GALAY_KERNEL_ASYNC_UDP_H
#define GALAY_KERNEL_ASYNC_UDP_H

#include "../common/defn.hpp"
#include "../common/error.h"
#include "../common/host.hpp"
#include "../common/handle_option.h"
#include "../core/awaitable.h"
#include "../core/io_scheduler.hpp"
#include <cstddef>
#include <expected>
#include <memory>

namespace galay::async
{
/**
 * @brief 异步UDP Socket类
 *
 * @details 封装底层 UDP socket 操作，提供 Task 友好的异步 IO 接口。
 * 内部包含：
 * - GHandle: 底层socket句柄
 * - IOScheduler*: IO调度器指针
 * - IOController: IO事件控制器
 *
 * @note
 * - 不可拷贝，仅支持移动语义；clone() 可派生共享同一底层句柄的副本
 * - 多个持有者共享同一 IOController（shared_ptr 引用计数，初始 1）；
 *   最后一个持有者 close()/析构时才真正关闭 socket
 * - 协程内可显式 co_await close() 释放持有并获取错误
 * - 所有异步操作需要在任务中使用 co_await
 * - UDP是无连接协议，不需要listen/accept/connect操作
 *
 * @see IOScheduler, HandleOption, Host
 */
class AsyncUdpSocket
{
public:
    /**
     * @brief 构造函数，创建Socket
     * @param type IP协议类型（IPV4/IPV6）
     * @note 兼容旧调用；创建失败时内部句柄保持 invalid。需要错误原因时请使用 create()。
     */
    explicit AsyncUdpSocket(galay::kernel::IPType type = galay::kernel::IPType::IPV4);

    /**
     * @brief 创建 UDP socket 并以返回值报告系统调用错误。
     * @param type IP 协议类型（IPV4/IPV6）
     * @return 成功返回可用 AsyncUdpSocket；socket() 失败时返回 IOError(kOpenFailed, errno)。
     * @note 不启动任何协程，也不阻塞；适合 C ABI/FFI 边界显式映射错误。
     */
    static std::expected<AsyncUdpSocket, galay::kernel::IOError> create(
        galay::kernel::IPType type = galay::kernel::IPType::IPV4);

    /**
     * @brief 从已有句柄构造Socket
     * @param handle 已有的socket句柄
     * @note 用于包装已存在的socket
     */
    explicit AsyncUdpSocket(GHandle handle);

    /**
     * @brief 析构函数
     * @note 递减共享计数；计数减到 0 时关闭仍由对象持有的 socket，若已通过 close() 关闭则不重复处理。
     */
    ~AsyncUdpSocket();

    /// @brief 禁用拷贝构造
    AsyncUdpSocket(const AsyncUdpSocket&) = delete;
    /// @brief 禁用拷贝赋值
    AsyncUdpSocket& operator=(const AsyncUdpSocket&) = delete;

    /**
     * @brief 移动构造函数
     * @param other 被移动的对象，移动后other变为无效状态
     */
    AsyncUdpSocket(AsyncUdpSocket&& other) noexcept;

    /**
     * @brief 移动赋值运算符
     * @param other 被移动的对象
     * @return 当前对象的引用
     */
    AsyncUdpSocket& operator=(AsyncUdpSocket&& other) noexcept;

    /**
     * @brief 获取底层socket句柄
     * @return GHandle 底层句柄，可用于底层操作；对象为 moved-from 状态时返回 invalid
     */
    GHandle handle() const {
        return m_controller ? m_controller->m_handle : GHandle::invalid();
    }

    /**
     * @brief 获取IO控制器指针
     * @return IOController* 内部IO控制器，用于高级操作
     */
    galay::kernel::IOController* controller() { return m_controller.get(); }


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
     * @brief 获取句柄选项配置器
     *
     * @return HandleOption 选项配置器对象
     *
     * @code
     * socket.option().handleReuseAddr();  // 设置地址重用
     * socket.option().handleNonBlock();   // 设置非阻塞
     * @endcode
     */
    galay::kernel::HandleOption option() { return galay::kernel::HandleOption(handle()); }

    /**
     * @brief 异步接收数据报
     *
     * @param buffer 接收缓冲区指针
     * @param length 缓冲区大小
     * @param from 输出参数，接收发送方地址信息，可为nullptr
     * @return RecvFromAwaitable 可等待对象，co_await后返回接收到的字节数
     *
     * @note
     * - UDP是数据报协议，每次接收一个完整的数据报
     * - 返回值为0表示收到合法的空数据报，不表示连接关闭
     * - 如果数据报大于缓冲区，超出部分会被丢弃
     * - 缓冲区生命周期必须持续到co_await完成
     *
     * @code
     * char buffer[1024];
     * Host from;
     * auto result = co_await socket.recvfrom(buffer, sizeof(buffer), &from);
     * if (result) {
     *     size_t bytes = result.value();
     *     // 处理数据，from包含发送方地址
     * }
     * @endcode
     */
    galay::kernel::RecvFromAwaitable recvfrom(
        char* buffer,
        size_t length,
        galay::kernel::Host* from);

    /**
     * @brief 异步发送数据报
     *
     * @param buffer 发送数据指针
     * @param length 数据长度
     * @param to 目标地址
     * @return SendToAwaitable 可等待对象，co_await后返回实际发送的字节数
     *
     * @note
     * - UDP是数据报协议，每次发送一个完整的数据报
     * - 缓冲区生命周期必须持续到co_await完成
     * - UDP不保证数据送达，也不保证顺序
     *
     * @code
     * const char* msg = "Hello";
     * Host target(IPType::IPV4, "127.0.0.1", 8080);
     * auto result = co_await socket.sendto(msg, strlen(msg), target);
     * if (result) {
     *     size_t sent = result.value();
     * }
     * @endcode
     */
    galay::kernel::SendToAwaitable sendto(
        const char* buffer,
        size_t length,
        const galay::kernel::Host& to);

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
    galay::kernel::CloseAwaitable close();  ///< 异步关闭 socket，恢复后返回关闭结果

    /*
     * @brief 获取IO控制器
     * @return IOController* IO控制器
     */
    galay::kernel::IOController* getController() { return m_controller.get(); }

    /**
     * @brief 克隆当前socket，共享底层句柄与IO控制器
     *
     * @return 与当前对象共享同一 IOController 的新 AsyncUdpSocket
     *
     * @note
     * - 返回的对象与当前对象共享持有计数（shared_ptr 引用计数加 1）
     * - clone 出的对象与原对象在同一 IO 调度器上使用（IOController 非线程安全）
     * - 任一持有者 close() 或析构仅释放自己的引用；最后一个持有者才真正关闭句柄
     */
    AsyncUdpSocket clone() const {
        return AsyncUdpSocket(m_controller);
    }

    /**
     * @brief 获取当前socket的共享持有计数
     * @return shared_ptr 引用计数；对象为 moved-from 状态时返回 0
     */
    int getSharedCount() const { return static_cast<int>(m_controller.use_count()); }

private:
    static std::expected<GHandle, galay::kernel::IOError> openHandle(galay::kernel::IPType type);  ///< 按协议版本创建底层 UDP socket

    /**
     * @brief 从共享控制器构造（仅 clone 使用）
     * @param controller 共享的 IO 控制器
     */
    explicit AsyncUdpSocket(std::shared_ptr<galay::kernel::IOController> controller) noexcept
        : m_controller(std::move(controller)) {}

    /**
     * @brief 释放本对象对共享控制器的持有
     * @note 递减共享计数；计数减到 0 且句柄仍有效时关闭句柄
     */
    void releaseSharedOwnership() noexcept;

private:
    std::shared_ptr<galay::kernel::IOController> m_controller;  ///< IO事件控制器；clone 共享同一实例
};

} // namespace galay::async

#endif // GALAY_KERNEL_ASYNC_UDP_H
