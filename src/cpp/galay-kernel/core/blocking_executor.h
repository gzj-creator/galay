/**
 * @file blocking_executor.h
 * @brief 自适应阻塞任务线程池
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供按需伸缩的线程池，当线程空闲超过配置的保活超时后自动收缩。
 * 由 Runtime::spawnBlocking() 用于卸载不可协程化的阻塞调用。
 */

#ifndef GALAY_KERNEL_BLOCKING_EXECUTOR_H
#define GALAY_KERNEL_BLOCKING_EXECUTOR_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <string_view>

namespace galay::kernel
{

/**
 * @brief 阻塞执行器提交错误类别。
 */
enum class BlockingExecutorErrorCode : uint8_t {
    kStopping  ///< 执行器正在停止，不能再接受新任务
};

/**
 * @brief 阻塞执行器错误对象。
 */
class BlockingExecutorError
{
public:
    explicit BlockingExecutorError(BlockingExecutorErrorCode error_code) noexcept
        : m_code(error_code)
    {
    }

    BlockingExecutorErrorCode code() const noexcept { return m_code; }  ///< 返回阻塞执行器错误类别
    std::string_view message() const noexcept
    {
        static constexpr std::array<std::string_view, static_cast<size_t>(BlockingExecutorErrorCode::kStopping) + 1> kMessages = {{
            "blocking executor is stopping and cannot accept new tasks"
        }};

        const auto index = static_cast<size_t>(m_code);
        if (index < kMessages.size()) {
            return kMessages[index];
        }
        return "unknown blocking executor error";
    }

private:
    BlockingExecutorErrorCode m_code;
};

/**
 * @brief 自适应阻塞任务执行器
 * @details 为 Runtime 的 `spawnBlocking()` 提供线程池，适合执行不可协程化的阻塞调用。
 */
class BlockingExecutor
{
public:
    BlockingExecutor();  ///< 使用默认线程数与空闲超时配置构造执行器
    BlockingExecutor(size_t minWorkers, size_t maxWorkers, std::chrono::milliseconds keepAlive);  ///< 自定义最小/最大线程数和空闲超时时间
    ~BlockingExecutor();  ///< 停止执行器并等待工作线程全部退出

    BlockingExecutor(const BlockingExecutor&) = delete;
    BlockingExecutor& operator=(const BlockingExecutor&) = delete;

    std::expected<void, BlockingExecutorError> submit(std::function<void()> task);  ///< 提交一个阻塞任务；必要时会拉起额外工作线程

private:
    void workerLoop(std::function<void()> initialTask);  ///< 工作线程主循环，持续拉取并执行阻塞任务
    void retireWorkerLocked();  ///< 在持锁状态下回收一个空闲工作线程计数
    static size_t defaultMaxWorkers();  ///< 根据当前机器并发度推导默认最大线程数

    size_t m_minWorkers;  ///< 最少保留的工作线程数
    size_t m_maxWorkers;  ///< 允许扩张到的最大工作线程数
    std::chrono::milliseconds m_keepAlive;  ///< 空闲线程超过该时间后允许退出

    std::mutex m_mutex;  ///< 保护任务队列和线程状态
    std::condition_variable m_taskCv;  ///< 新任务到达时唤醒工作线程
    std::condition_variable m_shutdownCv;  ///< 析构等待所有线程退出时使用
    std::deque<std::function<void()>> m_tasks;  ///< 待执行阻塞任务队列

    size_t m_workerCount{0};  ///< 当前已创建的工作线程数
    size_t m_idleWorkers{0};  ///< 当前空闲工作线程数
    bool m_stopping{false};  ///< 执行器是否处于停止中
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_BLOCKING_EXECUTOR_H
