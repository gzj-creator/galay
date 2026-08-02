#ifndef GALAY_BENCHMARK_SYNC_H
#define GALAY_BENCHMARK_SYNC_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <vector>

namespace galay::benchmark {

/**
 * @brief 可复用的 benchmark 完成闩锁。
 *
 * reset() 只能在上一轮没有并发 arrive()/wait()/ready() 时调用。调用方还必须保证
 * arrival 总数不超过 target；wait()/ready() 报告完成后，最后一个有效 arrival 已不再
 * 访问闩锁的 mutex 或 condition_variable，因此闩锁可以安全销毁。
 */
class CompletionLatch {
public:
    explicit CompletionLatch(std::size_t target = 0)
        : m_target(target),
          m_ready(target == 0) {}

    void reset(std::size_t target) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_target = target;
        m_count.store(0, std::memory_order_relaxed);
        m_ready = target == 0;
        if (m_ready) {
            m_cv.notify_all();
        }
    }

    void arrive(std::size_t count = 1) {
        if (count == 0) {
            return;
        }

        const std::size_t target = m_target;
        const std::size_t current =
            m_count.fetch_add(count, std::memory_order_acq_rel) + count;
        if (current < target) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_ready) {
            m_ready = true;
            m_cv.notify_all();
        }
    }

    [[nodiscard]] bool ready() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ready;
    }

    void wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_ready; });
    }

    template <typename Rep, typename Period>
    [[nodiscard]] bool waitFor(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this]() { return m_ready; });
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<std::size_t> m_count{0};
    std::size_t m_target{0};
    bool m_ready{true};
};

class StartGate {
public:
    void open() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_open = true;
        }
        m_cv.notify_all();
    }

    [[nodiscard]] bool isOpen() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_open;
    }

    void wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_open; });
    }

    template <typename Rep, typename Period>
    [[nodiscard]] bool waitFor(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this]() { return m_open; });
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_open{false};
};

template <typename TimeoutDuration, typename PollDuration = std::chrono::milliseconds>
bool waitForFlag(const std::atomic<bool>& flag,
                 TimeoutDuration timeout,
                 PollDuration poll_interval = std::chrono::milliseconds(1)) {
    if (flag.load(std::memory_order_acquire)) {
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(poll_interval);
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
    }

    return flag.load(std::memory_order_acquire);
}

constexpr int defaultBenchmarkSchedulerCount(unsigned hardware_threads,
                                             unsigned max_parallelism = 2) noexcept {
    const unsigned available_threads = hardware_threads == 0 ? 1u : hardware_threads;
    const unsigned capped_parallelism = max_parallelism == 0 ? 1u : max_parallelism;
    return static_cast<int>(std::min(available_threads, capped_parallelism));
}

template <typename T, typename Compare = std::less<T>>
T medianElement(std::vector<T> samples, Compare compare = Compare{}) {
    if (samples.empty()) {
        throw std::invalid_argument("medianElement requires at least one sample");
    }
    std::sort(samples.begin(), samples.end(), compare);
    return std::move(samples[samples.size() / 2]);
}

}  // namespace galay::benchmark

#endif
