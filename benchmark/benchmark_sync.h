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

class CompletionLatch {
public:
    explicit CompletionLatch(std::size_t target = 0)
        : m_target(target) {}

    void reset(std::size_t target) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_target = target;
        m_count.store(0, std::memory_order_release);
    }

    void arrive(std::size_t count = 1) {
        if (count == 0) {
            return;
        }
        const std::size_t current =
            m_count.fetch_add(count, std::memory_order_acq_rel) + count;
        if (current >= m_target) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cv.notify_all();
        }
    }

    [[nodiscard]] bool ready() const {
        return m_count.load(std::memory_order_acquire) >= m_target;
    }

    void wait() {
        if (ready()) {
            return;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return ready(); });
    }

    template <typename Rep, typename Period>
    [[nodiscard]] bool waitFor(std::chrono::duration<Rep, Period> timeout) {
        if (ready()) {
            return true;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this]() { return ready(); });
    }

private:
    std::atomic<std::size_t> m_count{0};
    std::size_t m_target{0};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
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
