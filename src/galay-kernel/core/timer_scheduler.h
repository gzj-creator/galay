/**
 * @file timer_scheduler.h
 * @brief 全局定时轮调度器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供全局的定时轮线程，用于处理 ComputeScheduler 的定时任务。
 * IOScheduler 的 IO 超时仍由各自的定时轮处理。
 *
 * 使用方式：
 * @code
 * // 启动全局定时轮（程序初始化时）
 * TimerScheduler::getInstance()->start();
 *
 * // 添加定时器
 * auto timer = std::make_shared<CBTimer>(100ms, []() {
 *     // 超时回调
 * });
 * TimerScheduler::getInstance()->addTimer(timer);
 *
 * // 停止（程序退出时）
 * TimerScheduler::getInstance()->stop();
 * @endcode
 */

#ifndef GALAY_KERNEL_TIMER_SCHEDULER_H
#define GALAY_KERNEL_TIMER_SCHEDULER_H

#include "galay-kernel/common/safetimer_mgr.hpp"
#include <thread>
#include <atomic>

namespace galay::kernel
{

/**
 * @brief 全局定时轮调度器（单例）
 *
 * @details 独立线程运行定时轮，用于处理非 IO 相关的定时任务。
 * 特点：
 * - 单例模式，全局唯一
 * - 独立线程驱动定时轮
 * - 线程安全的定时器添加接口
 */
class TimerScheduler
{
public:
    /**
     * @brief 获取单例实例
     * @return 全局定时轮调度器指针
     */
    static TimerScheduler* getInstance();

    // 禁止拷贝和移动
    TimerScheduler(const TimerScheduler&) = delete;
    TimerScheduler& operator=(const TimerScheduler&) = delete;
    TimerScheduler(TimerScheduler&&) = delete;
    TimerScheduler& operator=(TimerScheduler&&) = delete;

    /**
     * @brief 启动定时轮线程
     * @note 应在程序初始化时调用
     */
    void start();

    /**
     * @brief 停止定时轮线程
     * @note 应在程序退出时调用
     */
    void stop();

    /**
     * @brief 添加定时器（线程安全，无锁）
     * @param timer 定时器共享指针
     * @return true 添加成功，false 添加失败
     */
    bool addTimer(Timer::ptr timer);

    /**
     * @brief 批量添加定时器（线程安全，无锁）
     * @param timers 定时器列表
     * @return 成功添加的数量
     */
    size_t addTimerBatch(const std::vector<Timer::ptr>& timers);

    /**
     * @brief 检查是否正在运行
     * @return true 正在运行
     */
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    /**
     * @brief 获取定时轮 tick 间隔（纳秒）
     * @return tick 间隔
     */
    uint64_t tickDuration() const { return m_timerManager.during(); }

    /**
     * @brief 获取定时器总数（近似值）
     * @return 当前时间轮中挂起的定时器数量近似值
     */
    size_t size() const { return m_timerManager.size(); }

private:
    /**
     * @brief 构造单例对象
     * @note 只能通过 getInstance() 获取实例
     */
    TimerScheduler();

    /**
     * @brief 析构单例对象
     * @note 销毁前应先调用 stop() 停止后台线程
     */
    ~TimerScheduler();

    /**
     * @brief 定时轮线程主循环
     */
    void timerLoop();

private:
    std::thread m_thread;                           ///< 定时轮线程
    std::atomic<bool> m_running{false};             ///< 运行状态
    std::atomic<bool> m_stopFlag{false};            ///< 停止标志

    ThreadSafeTimerManager m_timerManager;          ///< 线程安全定时轮管理器
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_TIMER_SCHEDULER_H
