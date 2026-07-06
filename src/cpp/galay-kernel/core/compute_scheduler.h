/**
 * @file compute_scheduler.h
 * @brief 计算任务调度器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 基于单线程的计算任务调度器，用于处理 CPU 密集型任务。
 * 不涉及 IO 事件驱动，纯粹用于协程的计算任务调度。
 *
 * 使用方式：
 * @code
 * ComputeScheduler scheduler;
 * (void)scheduler.start();
 * scheduler.schedule(myComputeTask());
 * // ...
 * scheduler.stop();
 * @endcode
 */

#ifndef GALAY_KERNEL_COMPUTE_SCHEDULER_H
#define GALAY_KERNEL_COMPUTE_SCHEDULER_H

#include "scheduler.hpp"
#include "timer_scheduler.h"
#include <thread>
#include <atomic>
#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>

namespace galay::kernel
{

/**
 * @brief 计算任务
 *
 * @details 封装计算协程，用于跨线程传递。
 * 协程的原调度器通过 coro.belongScheduler() 获取。
 */
struct ComputeTask {
    TaskRef task;                  ///< 轻量任务引用
    bool is_stop_signal = false;    ///< 是否为停止信号
};

/**
 * @brief 计算任务调度器
 *
 * @details 基于单线程实现，适用于 CPU 密集型计算任务。
 * 特点：
 * - 单线程执行协程
 * - BlockingConcurrentQueue 实现高效阻塞等待
 * - 计算完成后自动 spawn 回原调度器
 *
 * @note 不支持 IO 操作，仅用于纯计算任务
 */
class ComputeScheduler : public Scheduler
{
public:
    /**
     * @brief 构造函数
     */
    ComputeScheduler();
    

    /**
     * @brief 析构函数
     * @note 会自动调用 stop()
     */
    ~ComputeScheduler();

    // 禁止拷贝
    ComputeScheduler(const ComputeScheduler&) = delete;
    ComputeScheduler& operator=(const ComputeScheduler&) = delete;


    /**
     * @brief 返回调度器类型
     * @return 固定返回 kComputeScheduler
     */
    SchedulerType type() override {
        return kComputeScheduler;
    }

    /**
     * @brief 启动调度器
     * @return 成功返回 void；当前实现不会产生 IOError
     * @note 创建工作线程并开始处理任务
     */
    std::expected<void, IOError> start() override;

    /**
     * @brief 停止调度器
     * @note 等待工作线程结束
     */
    void stop() override;

    /**
     * @brief 将计算任务排入工作线程
     * @param task 待执行的任务引用
     * @return true 任务已成功入队；false 任务无效或已绑定到其他调度器
     * @note 任务会在线程池中的计算线程恢复执行
     */
    bool schedule(TaskRef task) override;

    /**
     * @brief 将计算任务按延后语义排入工作线程
     * @param task 待执行的任务引用
     * @return true 任务已成功入队；false 任务无效或已绑定到其他调度器
     * @note 当前实现与 schedule() 共享同一工作队列，但保留独立语义入口
     */
    bool scheduleDeferred(TaskRef task) override;

    /**
     * @brief 立即执行任务（在当前线程）
     * @param task 要执行的任务
     * @return true 如果成功执行，false 如果任务已绑定到其他调度器
     */
    bool scheduleImmediately(TaskRef task) override;

    /**
     * @brief 检查调度器是否正在运行
     * @return true 如果正在运行
     */
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }


    /**
     * @brief 注册定时器
     * @details 添加任务量不大且数量不是特别多的定时任务，如IO超时，sleep等
     * @param timer 定时器共享指针
     * @return true 定时器已成功交给全局 TimerScheduler；false 添加失败
     */
    bool addTimer(Timer::ptr timer) override {
        return TimerScheduler::getInstance()->addTimer(timer);    
    }
private:
    /**
     * @brief 工作线程函数
     */
    void workerLoop();

private:
    std::thread m_thread;                                       ///< 工作线程
    moodycamel::BlockingConcurrentQueue<ComputeTask> m_queue;   ///< 任务队列（阻塞）
    std::atomic<bool> m_running{false};                         ///< 运行状态
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_COMPUTE_SCHEDULER_H
