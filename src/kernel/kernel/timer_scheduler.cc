/**
 * @file timer_scheduler.cc
 * @brief 全局定时轮调度器实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现 TimerScheduler 的单例获取、启停控制、定时器添加和定时轮主循环。
 * 定时轮在独立线程中运行，每个 tick 驱动 TimingWheel 处理到期定时器。
 */

#include "timer_scheduler.h"
#include <chrono>

namespace galay::kernel
{

/**
 * @brief 获取全局单例实例
 * @details 使用 Meyers' Singleton 模式（C++11 保证静态局部变量初始化的线程安全）。
 * @return TimerScheduler 单例指针
 */
TimerScheduler* TimerScheduler::getInstance()
{
    static TimerScheduler instance;
    return &instance;
}

/**
 * @brief 私有构造函数
 * @details 初始化 m_running 为 false、m_stopFlag 为 false，不启动线程。
 */
TimerScheduler::TimerScheduler()
{
}

/**
 * @brief 析构函数
 * @details 自动调用 stop() 确保后台线程在对象销毁前正确退出。
 */
TimerScheduler::~TimerScheduler()
{
    stop();
}

/**
 * @brief 启动定时轮后台线程
 * @details 使用 compare_exchange_strong 保证只启动一次。若已在运行则直接返回。
 * 启动后创建独立线程执行 timerLoop()。
 */
void TimerScheduler::start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // 已经在运行
    }

    m_stopFlag.store(false, std::memory_order_release);

    m_thread = std::thread([this]() {
        timerLoop();
    });
}

/**
 * @brief 停止定时轮后台线程
 * @details 设置停止标志后 join 等待线程退出。使用 compare_exchange_strong 保证只停止一次。
 */
void TimerScheduler::stop()
{
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;  // 已经停止
    }

    m_stopFlag.store(true, std::memory_order_release);

    // 等待线程结束
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

/**
 * @brief 添加单个定时器
 * @details 通过 ThreadSafeTimerManager 的无锁 push 将定时器加入队列。
 * 若调度器未运行或 timer 为空则返回 false。
 * @param timer 定时器共享指针
 * @return true 添加成功，false 添加失败
 */
bool TimerScheduler::addTimer(Timer::ptr timer)
{
    if (!timer || !m_running.load(std::memory_order_acquire)) {
        return false;
    }

    // 直接使用线程安全的 push（无锁）
    return m_timerManager.push(std::move(timer));
}

/**
 * @brief 批量添加定时器
 * @details 通过 ThreadSafeTimerManager 的 pushBatch 批量加入队列。
 * 若调度器未运行则返回 0。
 * @param timers 定时器列表
 * @return 成功添加的数量
 */
size_t TimerScheduler::addTimerBatch(const std::vector<Timer::ptr>& timers)
{
    if (!m_running.load(std::memory_order_acquire)) {
        return 0;
    }

    return m_timerManager.pushBatch(timers);
}

/**
 * @brief 定时轮主循环（运行在后台线程）
 * @details 每个 tick 间隔由 TimingWheel 的 during() 决定（纳秒级精度，取最小 1ms）。
 * 每次循环调用 m_timerManager.tick() 驱动定时轮处理到期定时器。
 * 退出前额外执行一次 tick 处理残余定时器。
 */
void TimerScheduler::timerLoop()
{
    // tick 间隔（纳秒转毫秒，至少 1ms）
    auto tickMs = std::max(1ULL, m_timerManager.during() / 1000000ULL);
    auto tickDuration = std::chrono::milliseconds(tickMs);

    while (!m_stopFlag.load(std::memory_order_acquire)) {
        // 驱动定时轮（内部会处理待添加的定时器）
        m_timerManager.tick();

        // 休眠一个 tick
        std::this_thread::sleep_for(tickDuration);
    }

    // 退出前最后处理一次
    m_timerManager.tick();
}

} // namespace galay::kernel
