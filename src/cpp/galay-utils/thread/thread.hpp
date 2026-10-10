#ifndef GALAY_UTILS_THREAD_HPP
#define GALAY_UTILS_THREAD_HPP

#include "../common/defn.hpp"
#include <galay/thirdparty/concurrentqueue/moodycamel/blockingconcurrentqueue.h>
#include <chrono>
#include <thread>
#include <vector>
#include <functional>
#include <future>
#include <atomic>
#include <memory>
#include <stdexcept>

namespace galay::utils {

/**
 * @file thread.hpp
 * @brief 线程池和任务等待器
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供高性能线程池（ThreadPool）和任务等待器（TaskWaiter）等并发工具。
 */

/**
 * @brief 高性能线程池
 * @details 使用 moodycamel::BlockingConcurrentQueue 传递任务，提交路径不使用互斥锁或条件变量；
 *          wait_all()/stop()/stop_now() 仍是阻塞线程 API。
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads = 0) {
        if (numThreads == 0) {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads == 0) {
                numThreads = 4;
            }
        }

        m_workers.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            m_workers.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        stop();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief 提交带返回值的异步任务
     * @tparam F 函数类型
     * @tparam Args 参数类型
     * @param f 待执行的函数
     * @param args 函数参数
     * @return 包含返回值的 future 对象
     */
    template<typename F, typename... Args>
    auto add_task(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<ReturnType> result = task->get_future();

        if (!enqueue_task([task]() { (*task)(); })) {
            throw std::runtime_error("ThreadPool is stopped");
        }

        return result;
    }

    /**
     * @brief 执行无返回值的异步任务
     * @tparam F 函数类型
     * @param f 待执行的函数
     * @return 无返回值
     */
    template<typename F>
    void execute(F&& f) {
        (void)enqueue_task(std::function<void()>(std::forward<F>(f)));
    }

    /**
     * @brief 获取线程数量
     * @return 对应的大小或数量
     */
    size_t thread_count() const { return m_workers.size(); }

    /**
     * @brief 获取待处理任务数量
     * @return 待处理任务数量
     */
    size_t pending_tasks() const {
        return m_pendingTasks.load(std::memory_order_acquire);
    }

    /**
     * @brief 判断线程池是否已停止
     * @return 满足所检查条件时返回 true，否则返回 false
     */
    bool is_stopped() const { return m_stopped; }

    /**
     * @brief 阻塞等待所有任务完成
     * @return 无返回值
     * @warning 会阻塞当前线程，不要在协程中使用
     */
    void wait_all() {
        size_t unfinished = m_unfinishedTasks.load(std::memory_order_acquire);
        while (unfinished != 0) {
            m_unfinishedTasks.wait(unfinished, std::memory_order_acquire);
            unfinished = m_unfinishedTasks.load(std::memory_order_acquire);
        }
    }

    /**
     * @brief 优雅停止线程池（等待所有任务完成后停止）
     * @return 无返回值
     */
    void stop() {
        if (m_stopped.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        m_accepting.store(false, std::memory_order_release);
        wait_for_submitters();
        wait_all();
        enqueue_stop_signals();

        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    /**
     * @brief 立即停止线程池（丢弃所有未完成任务）
     * @return 无返回值
     */
    void stop_now() {
        if (m_stopped.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        m_accepting.store(false, std::memory_order_release);
        wait_for_submitters();

        std::function<void()> discarded;
        while (m_tasks.try_dequeue(discarded)) {
            if (discarded) {
                m_pendingTasks.fetch_sub(1, std::memory_order_acq_rel);
                finish_queued_task();
            }
        }

        enqueue_stop_signals();

        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    bool begin_submit() {
        while (true) {
            if (!m_accepting.load(std::memory_order_acquire)) {
                return false;
            }

            m_submitters.fetch_add(1, std::memory_order_acq_rel);
            if (m_accepting.load(std::memory_order_acquire)) {
                return true;
            }

            end_submit();
        }
    }

    void end_submit() {
        if (m_submitters.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_submitters.notify_all();
        }
    }

    void wait_for_submitters() {
        size_t submitters = m_submitters.load(std::memory_order_acquire);
        while (submitters != 0) {
            m_submitters.wait(submitters, std::memory_order_acquire);
            submitters = m_submitters.load(std::memory_order_acquire);
        }
    }

    bool enqueue_task(std::function<void()> task) {
        if (!begin_submit()) {
            return false;
        }

        m_pendingTasks.fetch_add(1, std::memory_order_acq_rel);
        m_unfinishedTasks.fetch_add(1, std::memory_order_acq_rel);

        const bool enqueued = m_tasks.enqueue(std::move(task));
        end_submit();

        if (!enqueued) {
            m_pendingTasks.fetch_sub(1, std::memory_order_acq_rel);
            finish_queued_task();
            throw std::bad_alloc();
        }

        return true;
    }

    void enqueue_stop_signals() {
        for (size_t i = 0; i < m_workers.size(); ++i) {
            (void)m_tasks.enqueue(std::function<void()>{});
        }
    }

    void finish_queued_task() {
        if (m_unfinishedTasks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_unfinishedTasks.notify_all();
        }
    }

    void finish_active_task() {
        m_activeTasks.fetch_sub(1, std::memory_order_acq_rel);
        finish_queued_task();
    }

    void worker_loop() {
        while (true) {
            std::function<void()> task;
            m_tasks.wait_dequeue(task);

            if (!task) {
                if (m_stopped.load(std::memory_order_acquire)) {
                    return;
                }
                continue;
            }

            m_pendingTasks.fetch_sub(1, std::memory_order_acq_rel);
            m_activeTasks.fetch_add(1, std::memory_order_acq_rel);

            try {
                task();
            } catch (...) {
                finish_active_task();
                throw;
            }
            finish_active_task();
        }
    }

    std::vector<std::thread> m_workers;
    moodycamel::BlockingConcurrentQueue<std::function<void()>> m_tasks;
    std::atomic<bool> m_accepting{true};
    std::atomic<bool> m_stopped{false};
    std::atomic<size_t> m_submitters{0};
    std::atomic<size_t> m_pendingTasks{0};
    std::atomic<size_t> m_unfinishedTasks{0};
    std::atomic<size_t> m_activeTasks{0};
};

/**
 * @brief 任务等待器
 * @details 用于等待线程池中的多个任务完成。
 */
class TaskWaiter {
public:
    TaskWaiter() : m_count(0) {}

    /**
     * @brief 向线程池添加任务并跟踪计数
     * @tparam F 函数类型
     * @param pool 线程池引用
     * @param f 待执行的任务
     * @return 无返回值
     */
    template<typename F>
    void add_task(ThreadPool& pool, F&& f) {
        m_count.fetch_add(1, std::memory_order_acq_rel);
        try {
            (void)pool.add_task([this, func = std::forward<F>(f)]() {
                try {
                    func();
                } catch (...) {
                }
                finish_one();
            });
        } catch (...) {
            finish_one();
            throw;
        }
    }

    /**
     * @brief 阻塞等待所有任务完成
     * @return 无返回值
     * @warning 会阻塞当前线程，不要在协程中使用
     */
    void wait() {
        size_t count = m_count.load(std::memory_order_acquire);
        while (count != 0) {
            m_count.wait(count, std::memory_order_acquire);
            count = m_count.load(std::memory_order_acquire);
        }
    }

    /**
     * @brief 带超时的阻塞等待
     * @param timeout 超时时长
     * @return 在超时前完成等待时返回 true，否则返回 false
     * @warning 会阻塞当前线程，不要在协程中使用
     */
    template<typename Rep, typename Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (m_count.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

private:
    void finish_one() {
        if (m_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_count.notify_all();
        }
    }

    std::atomic<size_t> m_count;
};

} // namespace galay::utils

#endif // GALAY_UTILS_THREAD_HPP
