/**
 * @file blocking_executor.h
 * @brief 自适应阻塞任务线程池
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供按需伸缩的线程池，当线程空闲超过配置的保活超时后自动收缩。
 * 由 Runtime::spawn_blocking() 用于卸载不可协程化的阻塞调用。
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

    /**
     * @brief 返回阻塞执行器错误类别
     * @return 当前对象的类型、状态或错误码
     */
    BlockingExecutorErrorCode code() const noexcept { return m_code; }
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
 * @details 为 Runtime 的 `spawn_blocking()` 提供线程池，适合执行不可协程化的阻塞调用。
 */
class BlockingExecutor
{
public:
    BlockingExecutor();  ///< 使用默认线程数与空闲超时配置构造执行器
    /**
     * @brief 自定义最小/最大线程数和空闲超时时间
     * @param minWorkers 最少工作线程数量
     * @param maxWorkers 最多工作线程数量
     * @param keepAlive 空闲工作线程保留时长
     */
    BlockingExecutor(size_t minWorkers, size_t maxWorkers, std::chrono::milliseconds keepAlive);
    ~BlockingExecutor();  ///< 停止执行器并等待工作线程全部退出

    /**
     * @brief 停止接收任务并等待现有阻塞任务排空。
     *
     * Runtime 在停止 IO scheduler 前调用此方法，确保阻塞任务的异步
     * completion 能够唤醒仍在 scheduler 上等待的协程。
     * @return 无返回值
     */
    void stop() noexcept;

    BlockingExecutor(const BlockingExecutor&) = delete;
    BlockingExecutor& operator=(const BlockingExecutor&) = delete;

    /**
     * @brief 提交一个阻塞任务；必要时会拉起额外工作线程
     * @param task 协程任务
     * @return 成功时返回空值，失败时返回 BlockingExecutorError 错误
     */
    std::expected<void, BlockingExecutorError> submit(std::function<void()> task);

private:
    /**
     * @brief 工作线程主循环，持续拉取并执行阻塞任务
     * @param initial_task 初始任务
     * @return 无返回值
     */
    void worker_loop(std::function<void()> initial_task);
    /**
     * @brief 在持锁状态下回收一个空闲工作线程计数
     * @return 无返回值
     */
    void retire_worker_locked();
    /**
     * @brief 根据当前机器并发度推导默认最大线程数
     * @return size_t 操作结果
     */
    static size_t default_max_workers();

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
