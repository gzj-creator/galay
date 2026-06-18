/**
 * @file awaitable.cc
 * @brief 异步 IO awaitable 恢复实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现各 IO awaitable 类型的 await_resume() 方法，
 * 以及将 IOEventType 映射到对应 IOScheduler 注册方法的分发函数。
 */

#include "awaitable.h"
#include "galay-kernel/common/error.h"
#include "io_scheduler.hpp"
#include <cerrno>

namespace galay::kernel
{

namespace detail
{

/**
 * @brief 将 IO 事件注册分发到具体的 IOScheduler 后端
 *
 * @details 将通用 IOEventType 转换为对具体 IOScheduler 虚方法的调用（addAccept、addRecv 等）。
 *
 * @param scheduler  目标 IO 调度器（必须是 IOScheduler）
 * @param event      要注册的 IO 事件类型
 * @param controller 与 awaitable 关联的 IO 控制器
 * @return 1 表示 IO 立即完成，0 表示成功入队，负数表示错误
 */
int registerIOSchedulerEvent(Scheduler* scheduler,
                             IOEventType event,
                             IOController* controller) noexcept
{
    auto* io_scheduler = static_cast<IOScheduler*>(scheduler);
    switch (event) {
    case ACCEPT:
        return io_scheduler->addAccept(controller);
    case CONNECT:
        return io_scheduler->addConnect(controller);
    case RECV:
        return io_scheduler->addRecv(controller);
    case SEND:
        return io_scheduler->addSend(controller);
    case READV:
        return io_scheduler->addReadv(controller);
    case WRITEV:
        return io_scheduler->addWritev(controller);
    case SENDFILE:
        return io_scheduler->addSendFile(controller);
    case FILEREAD:
        return io_scheduler->addFileRead(controller);
    case FILEWRITE:
        return io_scheduler->addFileWrite(controller);
    case FILEWATCH:
        return io_scheduler->addFileWatch(controller);
    case RECVFROM:
        return io_scheduler->addRecvFrom(controller);
    case SENDTO:
        return io_scheduler->addSendTo(controller);
    case SEQUENCE:
        return io_scheduler->addSequence(controller);
    default:
        return -EINVAL;
    }
}

/**
 * @brief 在 IO 调度器上注册关闭操作
 *
 * @param scheduler  目标 IO 调度器
 * @param controller 需要关闭句柄的 IO 控制器
 * @return 0 表示成功，负数表示错误
 */
int registerIOSchedulerClose(Scheduler* scheduler,
                             IOController* controller) noexcept
{
    return static_cast<IOScheduler*>(scheduler)->addClose(controller);
}

int removeTimedOutIORegistration(Scheduler* scheduler, IOController* controller) noexcept
{
    if (scheduler == nullptr || scheduler->type() != kIOScheduler) {
        return 0;
    }
    return static_cast<IOScheduler*>(scheduler)->remove(controller);
}

} // namespace detail

/**
 * @brief 恢复 accept awaitable 并返回结果
 *
 * @details 在 io_uring 模式下，先重置 accept-result-assigned 标记，
 * 再委托给通用 resumeIOAwaitable 辅助函数。
 *
 * @return 成功时返回已接受的连接句柄，失败时返回 IOError
 */
std::expected<GHandle, IOError> AcceptAwaitable::await_resume() {
#ifdef USE_IOURING
    m_controller->m_accept_result_assigned = false;
#endif
    return detail::resumeIOAwaitable<ACCEPT>(*this);
}

/**
 * @brief 恢复 recv awaitable 并返回已接收字节数
 * @return 成功时返回已接收字节数，失败时返回 IOError
 */
std::expected<size_t, IOError> RecvAwaitable::await_resume() {
#ifdef USE_IOURING
    m_controller->m_recv_result_assigned = false;
#endif
    return detail::resumeIOAwaitable<RECV>(*this);
}

/**
 * @brief 恢复 send awaitable 并返回已发送字节数
 * @return 成功时返回已发送字节数，失败时返回 IOError
 */
std::expected<size_t, IOError> SendAwaitable::await_resume() {
    return detail::resumeIOAwaitable<SEND>(*this);
}

/**
 * @brief 恢复 readv awaitable 并返回已读取字节数
 * @return 成功时返回已读取字节数，失败时返回 IOError
 */
std::expected<size_t, IOError> ReadvAwaitable::await_resume() {
    return detail::resumeIOAwaitable<READV>(*this);
}

/**
 * @brief 恢复 writev awaitable 并返回已写入字节数
 * @return 成功时返回已写入字节数，失败时返回 IOError
 */
std::expected<size_t, IOError> WritevAwaitable::await_resume() {
    return detail::resumeIOAwaitable<WRITEV>(*this);
}

/**
 * @brief 恢复 connect awaitable 并返回连接结果
 * @return 成功或 IOError
 */
std::expected<void, IOError> ConnectAwaitable::await_resume() {
    return detail::resumeIOAwaitable<CONNECT>(*this);
}

/**
 * @brief 恢复 close awaitable 并返回关闭结果
 * @details 直接返回预先计算的关闭结果；实际的关闭操作已在 await_suspend 中同步完成。
 * @return 成功或 IOError
 */
std::expected<void, IOError> CloseAwaitable::await_resume() {
    return std::move(m_result);
}

/**
 * @brief 恢复文件读 awaitable 并返回已读取字节数
 * @return 成功时返回已读取字节数，失败时返回 IOError
 */
std::expected<size_t, IOError> FileReadAwaitable::await_resume() {
    return detail::resumeIOAwaitable<FILEREAD>(*this);
}

/**
 * @brief 恢复文件写 awaitable 并返回已写入字节数
 * @return 成功时返回已写入字节数，失败时返回 IOError
 */
std::expected<size_t, IOError> FileWriteAwaitable::await_resume() {
    return detail::resumeIOAwaitable<FILEWRITE>(*this);
}

/**
 * @brief 恢复 recvfrom awaitable 并返回已接收字节数
 * @return 成功时返回已接收字节数，失败时返回 IOError
 */
std::expected<size_t, IOError> RecvFromAwaitable::await_resume() {
    return detail::resumeIOAwaitable<RECVFROM>(*this);
}

/**
 * @brief 恢复 sendto awaitable 并返回已发送字节数
 * @return 成功时返回已发送字节数，失败时返回 IOError
 */
std::expected<size_t, IOError> SendToAwaitable::await_resume() {
    return detail::resumeIOAwaitable<SENDTO>(*this);
}

/**
 * @brief 恢复文件监控 awaitable 并返回监控结果
 * @return 包含触发事件详情的 FileWatchResult，失败时返回 IOError
 */
std::expected<FileWatchResult, IOError> FileWatchAwaitable::await_resume() {
    return detail::resumeIOAwaitable<FILEWATCH>(*this);
}

/**
 * @brief 恢复 sendfile awaitable 并返回已发送字节数
 * @return 成功时返回已发送字节数，失败时返回 IOError
 */
std::expected<size_t, IOError> SendFileAwaitable::await_resume() {
    return detail::resumeIOAwaitable<SENDFILE>(*this);
}

}
