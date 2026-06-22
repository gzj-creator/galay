/**
 * @file uring_scheduler.h
 * @brief 基于 Linux io_uring 的 IO 调度器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 将 IOUringReactor 与 SchedulerCore 事件循环骨架组合，
 * 提供 Linux io_uring 环境下的具体 IOScheduler 实现。
 * 由于 io_uring SQE 提交必须保持单线程，禁用了跨线程工作窃取。
 */

#ifndef GALAY_KERNEL_IOURING_SCHEDULER_H
#define GALAY_KERNEL_IOURING_SCHEDULER_H

#include "io_scheduler.hpp"
#include "uring_reactor.h"
#include "scheduler_core.h"
#include "wake_coordinator.h"

#ifdef USE_IOURING

#include <atomic>
#include <thread>
#include <cstdint>

#ifndef GALAY_SCHEDULER_QUEUE_DEPTH
#define GALAY_SCHEDULER_QUEUE_DEPTH 4096
#endif

#ifndef GALAY_SCHEDULER_BATCH_SIZE
#define GALAY_SCHEDULER_BATCH_SIZE 256
#endif

#ifndef GALAY_IOURING_WAIT_TIMEOUT_NS
#define GALAY_IOURING_WAIT_TIMEOUT_NS GALAY_KERNEL_IO_POLL_WAIT_MAX_NS
#endif

namespace galay::kernel
{

struct SchedulerTestAccess;

/**
 * @brief io_uring 调度器 (Linux)
 * @details 基于 io_uring 管理高吞吐异步 IO，并复用 IOScheduler 的任务注入模型。
 * 事件注册接口默认仅供所属调度器线程或其绑定协程调用。
 */
class IOUringScheduler: public IOScheduler
{
public:
    /**
     * @brief 构造 io_uring 调度器
     * @param queue_depth io_uring 提交/完成队列深度
     * @param batch_size 跨线程注入任务的批处理大小
     */
    IOUringScheduler(int queue_depth = GALAY_SCHEDULER_QUEUE_DEPTH,
                     int batch_size = GALAY_SCHEDULER_BATCH_SIZE);

    /**
     * @brief 析构调度器
     * @note 若事件循环仍在运行，析构会负责停止线程并释放 ring 资源
     */
    ~IOUringScheduler();

    IOUringScheduler(const IOUringScheduler&) = delete;
    IOUringScheduler& operator=(const IOUringScheduler&) = delete;

    std::expected<void, IOError> start() override;   ///< 初始化 io_uring 并启动事件循环线程
    void stop() override;    ///< 请求退出事件循环并等待工作线程结束
    void notify();           ///< 从其他线程唤醒可能阻塞在 completion wait 的调度器线程

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

    int remove(IOController* controller) override;        ///< 删除控制器关联的所有已注册操作；0=成功，<0=失败

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

private:
    std::atomic<bool> m_running;
    std::thread m_thread;

    int m_queue_depth;
    int m_batch_size;
    std::atomic<bool> m_sleeping{true};
    std::atomic<bool> m_wakeup_pending{false};

    IOSchedulerWorkerState m_worker;
    WakeCoordinator m_wake_coordinator;
    SchedulerCore m_core;
    IOUringReactor m_reactor;
    std::atomic<uint64_t> m_last_error_code{0};

private:
    /**
     * @brief io_uring 事件循环主函数
     * @note 仅在调度器工作线程中运行
     */
    void eventLoop();

    /**
     * @brief 处理跨线程注入和本地排队的待执行任务
     */
    void processPendingTasks();
};

}

#endif // USE_IOURING

#endif
