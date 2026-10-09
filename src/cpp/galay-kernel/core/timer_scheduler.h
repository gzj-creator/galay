/**
 * @file timer_scheduler.h
 * @brief 全局定时轮调度器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供全局的定时轮线程，用于处理 ParallelScheduler 的定时任务。
 * IOScheduler 的 IO 超时仍由各自的定时轮处理。
 *
 * 使用方式：
 * @code
 * // 启动全局定时轮（程序初始化时）
 * TimerScheduler::get_instance()->start();
 *
 * // 添加定时器
 * auto timer = std::make_shared<CBTimer>(100ms, []() {
 *     // 超时回调
 * });
 * TimerScheduler::get_instance()->add_timer(timer);
 *
 * // 停止（程序退出时）
 * TimerScheduler::get_instance()->stop();
 * @endcode
 */

#ifndef GALAY_KERNEL_TIMER_SCHEDULER_H
#define GALAY_KERNEL_TIMER_SCHEDULER_H

#include "../common/timer_manager_mt.hpp"
#include <thread>
#include <atomic>
#include <mutex>

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
    static TimerScheduler* get_instance();

    // 禁止拷贝和移动
    TimerScheduler(const TimerScheduler&) = delete;
    TimerScheduler& operator=(const TimerScheduler&) = delete;
    TimerScheduler(TimerScheduler&&) = delete;
    TimerScheduler& operator=(TimerScheduler&&) = delete;

    /**
     * @brief 获取定时轮使用权，首个使用者启动线程
     * @note 每次调用必须与 stop() 配对，在控制线程调用
     */
    void start();

    /**
     * @brief 释放定时轮使用权，最后一个使用者停止线程
     * @note 在控制线程调用
     */
    void stop();

    /**
     * @brief 添加定时器（线程安全，无锁）
     * @param timer 定时器共享指针
     * @return true 添加成功，false 添加失败
     */
    bool add_timer(Timer::ptr timer);

    /**
     * @brief 批量添加定时器（线程安全，无锁）
     * @param timers 定时器列表
     * @return 成功添加的数量
     */
    size_t add_timer_batch(const std::vector<Timer::ptr>& timers);

    /**
     * @brief 检查是否正在运行
     * @return true 正在运行
     */
    bool is_running() const { return m_running.load(std::memory_order_acquire); }

    /**
     * @brief 获取定时轮 tick 间隔（纳秒）
     * @return tick 间隔
     */
    uint64_t tick_duration() const { return m_timerManager.during(); }

    /**
     * @brief 获取定时器总数（近似值）
     * @return 当前时间轮中挂起的定时器数量近似值
     */
    size_t size() const { return m_timerManager.size(); }

private:
    /**
     * @brief 构造单例对象
     * @note 只能通过 get_instance() 获取实例
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
    void timer_loop();

private:
    // Runtime lifecycle calls already join worker threads; serialize only that path.
    std::mutex m_lifecycle_mutex;
    size_t m_users = 0;
    std::thread m_thread;                           ///< 定时轮线程
    ThreadSafeTimerManager m_timerManager;          ///< 线程安全定时轮管理器
    std::atomic<bool> m_running{false};             ///< 运行状态
    std::atomic<bool> m_stopFlag{false};            ///< 停止标志
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_TIMER_SCHEDULER_H
