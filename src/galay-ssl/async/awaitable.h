/**
 * @file awaitable.h
 * @brief SSL 异步操作的协程可等待对象
 * @author galay-ssl
 * @version 1.0.0
 *
 * @details 定义 SSL 接收、发送、握手和关闭操作的协程可等待对象，
 * 每个可等待对象内部封装对应的状态机，通过 co_await 驱动异步 SSL 操作。
 */

#ifndef GALAY_SSL_AWAITABLE_H
#define GALAY_SSL_AWAITABLE_H

#include "ssl_await.h"

namespace galay::ssl
{

/**
 * @brief SSL 异步接收可等待对象
 * @details 封装单次 SSL 接收操作的状态机，co_await 返回 std::expected<Bytes, SslError>
 */
struct SslRecvAwaitable
    : public SslStateMachineAwaitable<detail::SslSingleRecvMachine> {
    using Base = SslStateMachineAwaitable<detail::SslSingleRecvMachine>;

    /**
     * @brief 构造 SSL 接收可等待对象
     * @param controller IO 控制器
     * @param socket SSL Socket 指针
     * @param buffer 接收缓冲区
     * @param length 缓冲区大小
     */
    SslRecvAwaitable(IOController* controller, SslSocket* socket,
                     char* buffer, size_t length)
        : Base(controller, socket, detail::SslSingleRecvMachine(buffer, length)) {}

    using Base::await_ready;
    using Base::await_resume;
    using Base::await_suspend;
    using Base::timeout;
};

/**
 * @brief SSL 异步发送可等待对象
 * @details 封装单次 SSL 发送操作的状态机，co_await 返回 std::expected<size_t, SslError>
 */
struct SslSendAwaitable : public SslStateMachineAwaitable<detail::SslSingleSendMachine> {
    using Base = SslStateMachineAwaitable<detail::SslSingleSendMachine>;

    /**
     * @brief 构造 SSL 发送可等待对象
     * @param controller IO 控制器
     * @param socket SSL Socket 指针
     * @param buffer 发送数据指针
     * @param length 数据长度
     */
    SslSendAwaitable(IOController* controller, SslSocket* socket,
                     const char* buffer, size_t length)
        : Base(controller, socket, detail::SslSingleSendMachine(buffer, length)) {}

    using Base::await_ready;
    using Base::await_resume;
    using Base::await_suspend;
};

/**
 * @brief SSL 异步握手可等待对象
 * @details 封装单次 SSL 握手操作的状态机，co_await 返回 std::expected<void, SslError>
 */
struct SslHandshakeAwaitable : public SslStateMachineAwaitable<detail::SslSingleHandshakeMachine> {
    using Base = SslStateMachineAwaitable<detail::SslSingleHandshakeMachine>;

    /**
     * @brief 构造 SSL 握手可等待对象
     * @param controller IO 控制器
     * @param socket SSL Socket 指针
     */
    SslHandshakeAwaitable(IOController* controller, SslSocket* socket)
        : Base(controller, socket, detail::SslSingleHandshakeMachine{}) {}

    using Base::await_ready;
    using Base::await_resume;
    using Base::await_suspend;
};

/**
 * @brief SSL 异步关闭可等待对象
 * @details 封装单次 SSL 关闭操作的状态机，co_await 返回 std::expected<void, SslError>
 */
struct SslShutdownAwaitable : public SslStateMachineAwaitable<detail::SslSingleShutdownMachine> {
    using Base = SslStateMachineAwaitable<detail::SslSingleShutdownMachine>;

    /**
     * @brief 构造 SSL 关闭可等待对象
     * @param controller IO 控制器
     * @param socket SSL Socket 指针
     */
    SslShutdownAwaitable(IOController* controller, SslSocket* socket)
        : Base(controller, socket, detail::SslSingleShutdownMachine{}) {}

    using Base::await_ready;
    using Base::await_resume;
    using Base::await_suspend;
};

} // namespace galay::ssl

#endif // GALAY_SSL_AWAITABLE_H
