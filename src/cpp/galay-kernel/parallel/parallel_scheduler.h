/**
 * @file parallel_scheduler.h
 * @brief 计算任务调度器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 基于单线程的计算任务调度器，用于处理 CPU 密集型任务。
 * 不涉及 IO 事件驱动，纯粹用于协程的计算任务调度。
 *
 * 使用方式：
 * @code
 * ParallelScheduler scheduler;
 * (void)scheduler.start();
 * scheduler.schedule(myParallelTask());
 * // ...
 * scheduler.stop();
 * @endcode
 */

#ifndef GALAY_KERNEL_PARALLEL_SCHEDULER_H
#define GALAY_KERNEL_PARALLEL_SCHEDULER_H

#include "../core/scheduler.hpp"
#include "../core/timer_scheduler.h"
#include <cstddef>
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
struct ParallelTask {
    TaskRef task;  ///< 轻量任务引用
};

/**
 * @brief 不创建 coroutine frame 的纯计算工作项。
 *
 * @details `run` 在线程池 worker 上调用。`context` 的所有权由工作项持有的
 * `release` 回调表达；工作项被移动或销毁时恰好释放一次。该类型供并行
 * 组合子提交同步 callable，避免为每个计算节点额外创建 `Task<void>`。
 */
struct ParallelWorkItem {
    using Run = void (*)(void* context, std::size_t index) noexcept;
    using Release = void (*)(void* context) noexcept;

    constexpr ParallelWorkItem() noexcept = default;

    ParallelWorkItem(void* work_context,
                std::size_t work_index,
                Run work_run,
                Release work_release) noexcept
        : context(work_context),
          index(work_index),
          run(work_run),
          release(work_release)
    {
    }

    ParallelWorkItem(const ParallelWorkItem&) = delete;
    ParallelWorkItem& operator=(const ParallelWorkItem&) = delete;

    ParallelWorkItem(ParallelWorkItem&& other) noexcept
        : context(other.context),
          index(other.index),
          run(other.run),
          release(other.release)
    {
        other.clear();
    }

    ParallelWorkItem& operator=(ParallelWorkItem&& other) noexcept
    {
        if (this != &other) {
            reset();
            context = other.context;
            index = other.index;
            run = other.run;
            release = other.release;
            other.clear();
        }
        return *this;
    }

    ~ParallelWorkItem() { reset(); }

    bool valid() const noexcept { return context != nullptr && run != nullptr; }

    void reset() noexcept
    {
        if (context != nullptr && release != nullptr) {
            release(context);
        }
        clear();
    }

    void clear() noexcept
    {
        context = nullptr;
        index = 0;
        run = nullptr;
        release = nullptr;
    }

    void* context = nullptr;
    std::size_t index = 0;
    Run run = nullptr;
    Release release = nullptr;
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
class ParallelScheduler : public Scheduler
{
public:
    /**
     * @brief 构造函数
     */
    ParallelScheduler();
    

    /**
     * @brief 析构函数
     * @note 会自动调用 stop()
     */
    ~ParallelScheduler();

    // 禁止拷贝
    ParallelScheduler(const ParallelScheduler&) = delete;
    ParallelScheduler& operator=(const ParallelScheduler&) = delete;


    /**
     * @brief 返回调度器类型
     * @return 固定返回 kParallelScheduler
     */
    SchedulerType type() override {
        return kParallelScheduler;
    }

    /**
     * @brief 启动调度器
     * @return 成功返回 void；前一运行周期未完整排空恢复队列时返回 kNotReady
     * @note 创建工作线程并开始处理任务
     */
    std::expected<void, IOError> start() override;

    /**
     * @brief 停止调度器
     * @note 先拒绝新的恢复请求，再由工作线程排空已接纳任务并结束
     */
    void stop() override;

    /**
     * @brief 将计算任务排入工作线程
     * @param task 待执行的任务引用
     * @return true 任务已成功入队；false 任务无效或已绑定到其他调度器
     * @note 任务会在线程池中的计算线程恢复执行
     */
    bool schedule(TaskRef task) noexcept override;

    /**
     * @brief 将一个不拥有 coroutine frame 的同步计算工作项入队。
     * @return true 表示工作项已被 worker 接纳；调度器未运行或工作项无效时
     *         返回 false。
     * @note stop() 与入队并发时，已进入接纳协议的工作项仍会在 worker 退出前排空。
     */
    bool scheduleWork(ParallelWorkItem work) noexcept;

    /**
     * @brief 无分配接纳已停泊任务的恢复请求。
     * @return live scheduler 接管成功返回 true；未启动、已停止、任务无效或 owner
     *         不匹配返回 false。
     */
    bool scheduleResume(TaskRef task) noexcept override;

    /**
     * @brief 将计算任务按延后语义排入工作线程
     * @param task 待执行的任务引用
     * @return true 任务已成功入队；false 任务无效或已绑定到其他调度器
     * @note 当前实现与 schedule() 共享同一工作队列，但保留独立语义入口
     */
    bool scheduleDeferred(TaskRef task) noexcept override;

    /**
     * @brief 立即执行任务（在当前线程）
     * @param task 要执行的任务
     * @return true 如果成功执行，false 如果任务已绑定到其他调度器
     */
    bool scheduleImmediately(TaskRef task) noexcept override;

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
     * @brief 排空一次 resume admission 快照
     * @details 恢复期间新产生的请求留到下一轮；下一轮先尝试一个普通任务，
     *          普通队列为空时则立即继续恢复，兼顾公平性和连续 resume 吞吐。
     */
    void drainResumeQueue();

    /** @brief 工作线程函数 */
    void workerLoop();

private:
    std::thread m_thread;                                       ///< 工作线程
    moodycamel::BlockingConcurrentQueue<ParallelTask> m_queue;   ///< 任务队列（阻塞）
    moodycamel::BlockingConcurrentQueue<ParallelWorkItem> m_workQueue; ///< 同步计算工作队列
    detail::TaskResumeQueue m_resumeQueue;                       ///< Waker 专用无分配恢复队列
    std::atomic<bool> m_running{false};                         ///< 运行状态
    std::atomic<bool> m_worker_active{false};                    ///< worker 线程仍在执行（含停机排空阶段）
    std::atomic<std::size_t> m_submission_count{0};              ///< 正在进入普通队列的提交者数量
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_PARALLEL_SCHEDULER_H
