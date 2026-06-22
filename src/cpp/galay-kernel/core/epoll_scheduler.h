/**
 * @file epoll_scheduler.h
 * @brief 基于 Linux epoll 的 IO 调度器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 将 EpollReactor 与 SchedulerCore 事件循环骨架组合，
 * 提供 Linux epoll 环境下的具体 IOScheduler 实现。
 */

#ifndef GALAY_KERNEL_EPOLL_SCHEDULER_H
#define GALAY_KERNEL_EPOLL_SCHEDULER_H

#include "epoll_reactor.h"
#include "io_scheduler.hpp"
#include "scheduler_core.h"
#include "wake_coordinator.h"

#ifdef USE_EPOLL

#include <atomic>
#include <thread>
#include <cstdint>

#ifndef GALAY_SCHEDULER_MAX_EVENTS
#define GALAY_SCHEDULER_MAX_EVENTS 1024
#endif

#ifndef GALAY_SCHEDULER_BATCH_SIZE
#define GALAY_SCHEDULER_BATCH_SIZE 256
#endif

namespace galay::kernel
{

struct SchedulerTestAccess;

/**
 * @brief Epoll 调度器 (Linux)
 * @details 基于 epoll/eventfd/inotify/libaio 封装 IO 事件循环。
 * 公开方法除 `schedule*()` 外通常只应由绑定到该调度器的协程或调度器线程内部调用。
 */
class EpollScheduler: public IOScheduler
{
public:
    /**
     * @brief 构造 epoll 调度器
     * @param max_events epoll_wait 单次最多返回的事件数
     * @param batch_size 跨线程注入任务的批处理大小
     */
    EpollScheduler(int max_events = GALAY_SCHEDULER_MAX_EVENTS,
                   int batch_size = GALAY_SCHEDULER_BATCH_SIZE);

    /**
     * @brief 析构调度器
     * @note 若调度器仍在运行，析构过程会负责停止事件循环并回收线程
     */
    ~EpollScheduler();

    EpollScheduler(const EpollScheduler&) = delete;
    EpollScheduler& operator=(const EpollScheduler&) = delete;

    std::expected<void, IOError> start() override;   ///< 启动事件循环线程并初始化底层 reactor 资源
    void stop() override;    ///< 请求退出事件循环并等待工作线程结束
    void notify();           ///< 从其他线程唤醒可能阻塞在 epoll_wait 的调度器线程

    int addAccept(IOController* controller) override;     ///< 注册 accept 等待；1=立即完成，0=已挂起，<0=错误
    int addConnect(IOController* controller) override;    ///< 注册 connect 等待；1=立即完成，0=已挂起，<0=错误
    int addRecv(IOController* controller) override;       ///< 注册 recv 等待；1=立即完成，0=已挂起，<0=错误
    int addSend(IOController* controller) override;       ///< 注册 send 等待；1=立即完成，0=已挂起，<0=错误
    int addReadv(IOController* controller) override;      ///< 注册 readv 等待；1=立即完成，0=已挂起，<0=错误
    int addWritev(IOController* controller) override;     ///< 注册 writev 等待；1=立即完成，0=已挂起，<0=错误
    int addClose(IOController* controller) override;      ///< 注册关闭句柄操作；0=成功提交，<0=失败

    int addFileRead(IOController* controller) override;   ///< 注册文件读取等待；1=立即完成，0=已挂起，<0=错误
    int addFileWrite(IOController* controller) override;  ///< 注册文件写入等待；1=立即完成，0=已挂起，<0=错误

    int addRecvFrom(IOController* controller) override;   ///< 注册 recvfrom 等待；1=立即完成，0=已挂起，<0=错误
    int addSendTo(IOController* controller) override;     ///< 注册 sendto 等待；1=立即完成，0=已挂起，<0=错误

    int addFileWatch(IOController* controller) override;  ///< 注册文件监控等待；1=立即完成，0=已挂起，<0=错误

    int addSendFile(IOController* controller) override;   ///< 注册 sendfile 等待；1=立即完成，0=已挂起，<0=错误

    int addSequence(IOController* controller) override;   ///< 注册组合式序列 IO；1=立即完成，0=已挂起，<0=错误

    int remove(IOController* controller) override;        ///< 删除控制器关联的所有已注册事件；0=成功，<0=失败
    std::optional<IOError> lastError() const override;    ///< 返回最近一次内部错误；无错误时返回 std::nullopt

    bool schedule(TaskRef task) override;                 ///< 从任意线程注入任务；成功时必要时会唤醒事件循环
    bool scheduleDeferred(TaskRef task) override;         ///< 以延后语义注入任务；当前仍使用同一注入队列
    bool scheduleImmediately(TaskRef task) override;      ///< 在调度器线程内立刻恢复任务；跨线程调用会失败

    void configureStealDomain(std::span<IOScheduler* const> siblings,
                              size_t self_index) override
    {
        IOScheduler::configureStealDomain(siblings, self_index);
        m_worker.configureStealDomain(self_index, siblings);
    }

    IOSchedulerWorkerState* stealWorkerState() noexcept override
    {
        return &m_worker;
    }

    IOSchedulerStealStats stealStats() const noexcept override
    {
        return m_worker.snapshotStealStats();
    }

    friend struct SchedulerTestAccess;

protected:
    std::atomic<bool> m_running;
    std::thread m_thread;

    int m_max_events;
    int m_batch_size;

    std::atomic<uint64_t> m_last_error_code{0};
    std::atomic<bool> m_sleeping{true};
    std::atomic<bool> m_wakeup_pending{false};

    IOSchedulerWorkerState m_worker;
    WakeCoordinator m_wake_coordinator;
    SchedulerCore m_core;
    EpollReactor m_reactor;

private:
    /**
     * @brief epoll 事件循环主函数
     * @note 仅在调度器工作线程中运行
     */
    void eventLoop();

    /**
     * @brief 处理跨线程注入和本地排队的待执行任务
     */
    void processPendingTasks();
};

}

#endif // USE_EPOLL

#endif
