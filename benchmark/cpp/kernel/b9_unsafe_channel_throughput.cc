/**
 * @file b9_unsafe.cc
 * @brief 用途：压测 `galay::spsc::UnboundedChannel` 在同线程协程通信场景下的极限性能。
 * 关键覆盖点：同调度器吞吐、延迟采样、与 `galay::mpsc::UnboundedChannel` 的参考量级对照。
 * 通过条件：所有测量样本完成并输出统计结果，进程无异常退出。
 */

#include <atomic>
#include <chrono>
#include <vector>
#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/compute_scheduler.h>
#include "test/cpp/common/stdout_log.h"

using namespace galay::kernel;
using namespace std::chrono_literals;

// ============== 压测参数 ==============
constexpr int64_t THROUGHPUT_MESSAGES = 1000000;
constexpr int64_t LATENCY_MESSAGES = 100000;
constexpr std::size_t THROUGHPUT_SAMPLE_COUNT = 5;
constexpr auto THROUGHPUT_MIN_SAMPLE_DURATION = 300ms;

// ============== 全局计数器 ==============
std::atomic<int64_t> g_sent{0};
std::atomic<int64_t> g_received{0};
std::atomic<int64_t> g_sum{0};
std::atomic<int64_t> g_latency_sum_ns{0};
std::atomic<int64_t> g_latency_count{0};
std::atomic<bool> g_consumer_done{false};
std::atomic<bool> g_producer_done{false};
std::atomic<bool> g_benchmark_failed{false};

void resetCounters() {
    g_sent = 0;
    g_received = 0;
    g_sum = 0;
    g_latency_sum_ns = 0;
    g_latency_count = 0;
    g_consumer_done = false;
    g_producer_done = false;
    g_benchmark_failed = false;
}

// ============== 消息结构 ==============
struct TimestampedMessage {
    int64_t id;
    std::chrono::steady_clock::time_point send_time;
};

struct ThroughputMeasurement {
    double elapsed_ms;
    double throughput;
};

struct ThroughputSample {
    double elapsed_ms;
    double throughput;
    int64_t received;
    int64_t sum;
};

ThroughputMeasurement measureThroughput(int64_t message_count,
                                        std::chrono::steady_clock::duration elapsed) {
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return {
        .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
        .throughput = elapsed_ns > 0
            ? (static_cast<double>(message_count) * 1'000'000'000.0 / elapsed_ns)
            : 0.0,
    };
}

template <typename Runner>
ThroughputSample measureThroughputSample(int64_t message_count, Runner&& runner) {
    int64_t sample_message_count = message_count;
    while (true) {
        const auto elapsed = runner(sample_message_count);
        const auto measurement = measureThroughput(sample_message_count, elapsed);
        if (elapsed >= THROUGHPUT_MIN_SAMPLE_DURATION ||
            g_benchmark_failed.load(std::memory_order_acquire)) {
            return {
                .elapsed_ms = measurement.elapsed_ms,
                .throughput = measurement.throughput,
                .received = g_received.load(std::memory_order_relaxed),
                .sum = g_sum.load(std::memory_order_relaxed),
            };
        }
        sample_message_count *= 2;
    }
}

// ============== galay::spsc::UnboundedChannel 消费者协程 ==============

Task<void> unsafeSimpleConsumer(galay::spsc::UnboundedChannel<int64_t>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        if (g_benchmark_failed.load(std::memory_order_acquire) &&
            g_producer_done.load(std::memory_order_acquire)) {
            break;
        }
        auto value = co_await channel->recv();
        if (!value) {
            g_benchmark_failed.store(true, std::memory_order_release);
            break;
        }
        ++received;
        sum += *value;
    }
    g_received.store(received, std::memory_order_relaxed);
    g_sum.store(sum, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

Task<void> unsafeBatchConsumer(galay::spsc::UnboundedChannel<int64_t>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        if (g_benchmark_failed.load(std::memory_order_acquire) &&
            g_producer_done.load(std::memory_order_acquire)) {
            break;
        }
        auto batch = co_await channel->recvBatch(256);
        if (!batch) {
            g_benchmark_failed.store(true, std::memory_order_release);
            break;
        }
        for (int64_t v : *batch) {
            sum += v;
        }
        received += batch->size();
    }
    g_received.store(received, std::memory_order_relaxed);
    g_sum.store(sum, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

Task<void> unsafeBatchedConsumer(galay::spsc::UnboundedChannel<int64_t>* channel, int64_t expected_count, int64_t batch_limit) {
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        if (g_benchmark_failed.load(std::memory_order_acquire) &&
            g_producer_done.load(std::memory_order_acquire)) {
            break;
        }
        auto batch = co_await channel->recvBatched(batch_limit);
        if (!batch) {
            g_benchmark_failed.store(true, std::memory_order_release);
            break;
        }
        for (int64_t v : *batch) {
            sum += v;
        }
        received += batch->size();
    }
    g_received.store(received, std::memory_order_relaxed);
    g_sum.store(sum, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

Task<void> unsafeLatencyConsumer(galay::spsc::UnboundedChannel<TimestampedMessage>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t latency_sum_ns = 0;
    while (received < expected_count) {
        if (g_benchmark_failed.load(std::memory_order_acquire) &&
            g_producer_done.load(std::memory_order_acquire)) {
            break;
        }
        auto msg = co_await channel->recv();
        if (!msg) {
            g_benchmark_failed.store(true, std::memory_order_release);
            break;
        }
        auto now = std::chrono::steady_clock::now();
        auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - msg->send_time).count();
        latency_sum_ns += latency_ns;
        ++received;
    }
    g_received.store(received, std::memory_order_relaxed);
    g_latency_sum_ns.store(latency_sum_ns, std::memory_order_relaxed);
    g_latency_count.store(received, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

// ============== galay::spsc::UnboundedChannel 生产者协程 ==============

Task<void> unsafeSimpleProducer(galay::spsc::UnboundedChannel<int64_t>* channel, int64_t count) {
    int64_t sent = 0;
    for (int64_t i = 0; i < count; ++i) {
        const bool flush_tail = i + 1 == count;
        if (!channel->send(i, flush_tail)) {
            LogError("  spsc producer send failed after {} messages", sent);
            g_benchmark_failed.store(true, std::memory_order_release);
            break;
        }
        ++sent;
        if (i % 1000 == 0) {
            co_yield true;  // 让出执行权
        }
    }
    g_sent.store(sent, std::memory_order_relaxed);
    g_producer_done.store(true, std::memory_order_release);
    co_return;
}

Task<void> unsafeLatencyProducer(galay::spsc::UnboundedChannel<TimestampedMessage>* channel, int64_t count) {
    int64_t sent = 0;
    for (int64_t i = 0; i < count; ++i) {
        TimestampedMessage msg;
        msg.id = i;
        msg.send_time = std::chrono::steady_clock::now();
        if (!channel->send(std::move(msg))) {
            LogError("  spsc latency producer send failed after {} messages", sent);
            g_benchmark_failed.store(true, std::memory_order_release);
            break;
        }
        ++sent;
        if (i % 100 == 0) {
            co_yield true;
        }
    }
    g_sent.store(sent, std::memory_order_relaxed);
    g_producer_done.store(true, std::memory_order_release);
    co_return;
}

// ============== galay::mpsc::UnboundedChannel 消费者协程（用于对比）==============

Task<void> mpscSimpleConsumer(galay::mpsc::UnboundedChannel<int64_t>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        if (g_benchmark_failed.load(std::memory_order_acquire) &&
            g_producer_done.load(std::memory_order_acquire)) {
            break;
        }
        auto value = co_await channel->recv();
        if (!value) {
            g_benchmark_failed.store(true, std::memory_order_release);
            break;
        }
        ++received;
        sum += *value;
    }
    g_received.store(received, std::memory_order_relaxed);
    g_sum.store(sum, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

// ============== galay::mpsc::UnboundedChannel 生产者协程（用于对比）==============

Task<void> mpscSimpleProducer(galay::mpsc::UnboundedChannel<int64_t>* channel, int64_t count) {
    int64_t sent = 0;
    for (int64_t i = 0; i < count; ++i) {
        if (!channel->send(i)) {
            LogError("  mpsc producer send failed after {} messages", sent);
            g_benchmark_failed.store(true, std::memory_order_release);
            if (!channel->close() && !channel->isClosed()) {
                LogError("  mpsc channel close failed after send failure");
            }
            break;
        }
        ++sent;
        if (i % 1000 == 0) {
            co_yield true;
        }
    }
    g_sent.store(sent, std::memory_order_relaxed);
    g_producer_done.store(true, std::memory_order_release);
    co_return;
}

// ============== 压测函数 ==============

// 1. galay::spsc::UnboundedChannel 单生产者吞吐量测试
void benchUnsafeChannelThroughput(int64_t message_count) {
    LogInfo("--- galay::spsc::UnboundedChannel Throughput Test ({} messages) ---", message_count);
    std::vector<ThroughputSample> samples;
    samples.reserve(THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        samples.push_back(measureThroughputSample(message_count, [&](int64_t sample_message_count) {
            resetCounters();

            galay::spsc::UnboundedChannel<int64_t> channel;
            ComputeScheduler scheduler;

            auto started = scheduler.start();
            if (!started) {
                LogError("  failed to start spsc throughput scheduler");
                g_benchmark_failed.store(true, std::memory_order_release);
                return std::chrono::steady_clock::duration::zero();
            }

            const auto start = std::chrono::steady_clock::now();

            if (!scheduleTask(scheduler, unsafeSimpleConsumer(&channel, sample_message_count)) ||
                !scheduleTask(scheduler, unsafeSimpleProducer(&channel, sample_message_count))) {
                LogError("  failed to schedule spsc throughput tasks");
                g_benchmark_failed.store(true, std::memory_order_release);
                scheduler.stop();
                return std::chrono::steady_clock::duration::zero();
            }

            while (!g_consumer_done) {
                std::this_thread::sleep_for(1ms);
            }

            const auto elapsed = std::chrono::steady_clock::now() - start;
            scheduler.stop();
            return elapsed;
        }));
        if (g_benchmark_failed.load(std::memory_order_acquire)) {
            return;
        }
    }

    const auto median_sample = galay::benchmark::medianElement(
        std::move(samples),
        [](const ThroughputSample& lhs, const ThroughputSample& rhs) {
            return lhs.throughput < rhs.throughput;
        });
    const int64_t expected_sum = (median_sample.received - 1) * median_sample.received / 2;
    const bool correct =
        (median_sample.received > 0) && (median_sample.sum == expected_sum);

    LogInfo("  sent={}, received={}, time={}ms, throughput={:.0f} msg/s",
            median_sample.received,
            median_sample.received,
            median_sample.elapsed_ms,
            median_sample.throughput);
    LogInfo("  sum={} (expected {}), correct={}",
            median_sample.sum, expected_sum, correct ? "YES" : "NO");
}

// 2. galay::spsc::UnboundedChannel 批量接收吞吐量测试
void benchUnsafeChannelBatchThroughput(int64_t message_count) {
    LogInfo("--- galay::spsc::UnboundedChannel Batch Receive Throughput Test ({} messages) ---", message_count);
    std::vector<ThroughputSample> samples;
    samples.reserve(THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        samples.push_back(measureThroughputSample(message_count, [&](int64_t sample_message_count) {
            resetCounters();

            galay::spsc::UnboundedChannel<int64_t> channel;
            ComputeScheduler scheduler;

            auto started = scheduler.start();
            if (!started) {
                LogError("  failed to start spsc batch scheduler");
                g_benchmark_failed.store(true, std::memory_order_release);
                return std::chrono::steady_clock::duration::zero();
            }

            const auto start = std::chrono::steady_clock::now();

            if (!scheduleTask(scheduler, unsafeBatchConsumer(&channel, sample_message_count)) ||
                !scheduleTask(scheduler, unsafeSimpleProducer(&channel, sample_message_count))) {
                LogError("  failed to schedule spsc batch tasks");
                g_benchmark_failed.store(true, std::memory_order_release);
                scheduler.stop();
                return std::chrono::steady_clock::duration::zero();
            }

            while (!g_consumer_done) {
                std::this_thread::sleep_for(1ms);
            }

            const auto elapsed = std::chrono::steady_clock::now() - start;
            scheduler.stop();
            return elapsed;
        }));
        if (g_benchmark_failed.load(std::memory_order_acquire)) {
            return;
        }
    }

    const auto median_sample = galay::benchmark::medianElement(
        std::move(samples),
        [](const ThroughputSample& lhs, const ThroughputSample& rhs) {
            return lhs.throughput < rhs.throughput;
        });
    const int64_t expected_sum = (median_sample.received - 1) * median_sample.received / 2;
    const bool correct =
        (median_sample.received > 0) && (median_sample.sum == expected_sum);

    LogInfo("  sent={}, received={}, time={}ms, throughput={:.0f} msg/s",
            median_sample.received,
            median_sample.received,
            median_sample.elapsed_ms,
            median_sample.throughput);
    LogInfo("  sum={} (expected {}), correct={}",
            median_sample.sum, expected_sum, correct ? "YES" : "NO");
}

// 2b. galay::spsc::UnboundedChannel recvBatched 攒批接收吞吐量测试
void benchUnsafeChannelBatchedThroughput(int64_t message_count, int64_t batch_limit) {
    LogInfo("--- galay::spsc::UnboundedChannel recvBatched Throughput Test ({} messages, limit={}) ---",
            message_count, batch_limit);
    std::vector<ThroughputSample> samples;
    samples.reserve(THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        samples.push_back(measureThroughputSample(message_count, [&](int64_t sample_message_count) {
            resetCounters();

            galay::spsc::UnboundedChannel<int64_t> channel;
            ComputeScheduler scheduler;

            auto started = scheduler.start();
            if (!started) {
                LogError("  failed to start spsc recvBatched scheduler");
                g_benchmark_failed.store(true, std::memory_order_release);
                return std::chrono::steady_clock::duration::zero();
            }

            const auto start = std::chrono::steady_clock::now();

            if (!scheduleTask(
                    scheduler,
                    unsafeBatchedConsumer(&channel, sample_message_count, batch_limit)) ||
                !scheduleTask(scheduler, unsafeSimpleProducer(&channel, sample_message_count))) {
                LogError("  failed to schedule spsc recvBatched tasks");
                g_benchmark_failed.store(true, std::memory_order_release);
                scheduler.stop();
                return std::chrono::steady_clock::duration::zero();
            }

            while (!g_consumer_done) {
                std::this_thread::sleep_for(1ms);
            }

            const auto elapsed = std::chrono::steady_clock::now() - start;
            scheduler.stop();
            return elapsed;
        }));
        if (g_benchmark_failed.load(std::memory_order_acquire)) {
            return;
        }
    }

    const auto median_sample = galay::benchmark::medianElement(
        std::move(samples),
        [](const ThroughputSample& lhs, const ThroughputSample& rhs) {
            return lhs.throughput < rhs.throughput;
        });
    const int64_t expected_sum = (median_sample.received - 1) * median_sample.received / 2;
    const bool correct =
        (median_sample.received > 0) && (median_sample.sum == expected_sum);

    LogInfo("  sent={}, received={}, time={}ms, throughput={:.0f} msg/s",
            median_sample.received,
            median_sample.received,
            median_sample.elapsed_ms,
            median_sample.throughput);
    LogInfo("  sum={} (expected {}), correct={}",
            median_sample.sum, expected_sum, correct ? "YES" : "NO");
}

// 3. galay::spsc::UnboundedChannel 延迟测试
void benchUnsafeChannelLatency(int64_t message_count) {
    LogInfo("--- galay::spsc::UnboundedChannel Latency Test ({} messages) ---", message_count);
    resetCounters();

    galay::spsc::UnboundedChannel<TimestampedMessage> channel;
    ComputeScheduler scheduler;

    auto started = scheduler.start();
    if (!started) {
        LogError("  failed to start spsc latency scheduler");
        g_benchmark_failed.store(true, std::memory_order_release);
        return;
    }

    if (!scheduleTask(scheduler, unsafeLatencyConsumer(&channel, message_count)) ||
        !scheduleTask(scheduler, unsafeLatencyProducer(&channel, message_count))) {
        LogError("  failed to schedule spsc latency tasks");
        g_benchmark_failed.store(true, std::memory_order_release);
        scheduler.stop();
        return;
    }

    while (!g_consumer_done) {
        std::this_thread::sleep_for(1ms);
    }

    scheduler.stop();

    const int64_t latency_count = g_latency_count.load(std::memory_order_relaxed);
    if (latency_count == 0) {
        LogError("  spsc latency benchmark received no messages");
        g_benchmark_failed.store(true, std::memory_order_release);
        return;
    }
    double avg_latency_us =
        static_cast<double>(g_latency_sum_ns.load(std::memory_order_relaxed)) /
        latency_count / 1000.0;

    LogInfo("  messages={}, avg_latency={:.2f}us", g_received.load(), avg_latency_us);
}

// 4. galay::mpsc::UnboundedChannel 吞吐量测试（同调度器，用于对比）
void benchMpscChannelThroughput(int64_t message_count) {
    LogInfo("--- galay::mpsc::UnboundedChannel Throughput Test (same scheduler, {} messages) ---", message_count);
    std::vector<ThroughputSample> samples;
    samples.reserve(THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        samples.push_back(measureThroughputSample(message_count, [&](int64_t sample_message_count) {
            resetCounters();

            galay::mpsc::UnboundedChannel<int64_t> channel;
            ComputeScheduler scheduler;

            auto started = scheduler.start();
            if (!started) {
                LogError("  failed to start mpsc throughput scheduler");
                g_benchmark_failed.store(true, std::memory_order_release);
                return std::chrono::steady_clock::duration::zero();
            }

            const auto start = std::chrono::steady_clock::now();

            if (!scheduleTask(scheduler, mpscSimpleConsumer(&channel, sample_message_count)) ||
                !scheduleTask(scheduler, mpscSimpleProducer(&channel, sample_message_count))) {
                LogError("  failed to schedule mpsc throughput tasks");
                g_benchmark_failed.store(true, std::memory_order_release);
                scheduler.stop();
                return std::chrono::steady_clock::duration::zero();
            }

            while (!g_consumer_done) {
                std::this_thread::sleep_for(1ms);
            }

            const auto elapsed = std::chrono::steady_clock::now() - start;
            scheduler.stop();
            return elapsed;
        }));
        if (g_benchmark_failed.load(std::memory_order_acquire)) {
            return;
        }
    }

    const auto median_sample = galay::benchmark::medianElement(
        std::move(samples),
        [](const ThroughputSample& lhs, const ThroughputSample& rhs) {
            return lhs.throughput < rhs.throughput;
        });
    const int64_t expected_sum = (median_sample.received - 1) * median_sample.received / 2;
    const bool correct =
        (median_sample.received > 0) && (median_sample.sum == expected_sum);

    LogInfo("  sent={}, received={}, time={}ms, throughput={:.0f} msg/s",
            median_sample.received,
            median_sample.received,
            median_sample.elapsed_ms,
            median_sample.throughput);
    LogInfo("  sum={} (expected {}), correct={}",
            median_sample.sum, expected_sum, correct ? "YES" : "NO");
}

// 5. 性能对比总结
void benchComparison(int64_t message_count) {
    LogInfo("\n=== Performance Comparison ({} messages) ===", message_count);

    // galay::spsc::UnboundedChannel 测试
    resetCounters();
    galay::spsc::UnboundedChannel<int64_t> unsafeChannel;
    ComputeScheduler scheduler1;

    auto started1 = scheduler1.start();
    if (!started1) {
        LogError("  failed to start spsc comparison scheduler");
        g_benchmark_failed.store(true, std::memory_order_release);
        return;
    }
    auto start1 = std::chrono::steady_clock::now();
    if (!scheduleTask(scheduler1, unsafeSimpleConsumer(&unsafeChannel, message_count)) ||
        !scheduleTask(scheduler1, unsafeSimpleProducer(&unsafeChannel, message_count))) {
        LogError("  failed to schedule spsc comparison tasks");
        g_benchmark_failed.store(true, std::memory_order_release);
        scheduler1.stop();
        return;
    }
    while (!g_consumer_done) {
        std::this_thread::sleep_for(1ms);
    }
    auto elapsed1 = std::chrono::steady_clock::now() - start1;
    const auto measurement1 = measureThroughput(message_count, elapsed1);
    scheduler1.stop();
    if (g_benchmark_failed.load(std::memory_order_acquire)) {
        return;
    }

    // galay::mpsc::UnboundedChannel 测试
    resetCounters();
    galay::mpsc::UnboundedChannel<int64_t> mpscChannel;
    ComputeScheduler scheduler2;

    auto started2 = scheduler2.start();
    if (!started2) {
        LogError("  failed to start mpsc comparison scheduler");
        g_benchmark_failed.store(true, std::memory_order_release);
        return;
    }
    auto start2 = std::chrono::steady_clock::now();
    if (!scheduleTask(scheduler2, mpscSimpleConsumer(&mpscChannel, message_count)) ||
        !scheduleTask(scheduler2, mpscSimpleProducer(&mpscChannel, message_count))) {
        LogError("  failed to schedule mpsc comparison tasks");
        g_benchmark_failed.store(true, std::memory_order_release);
        scheduler2.stop();
        return;
    }
    while (!g_consumer_done) {
        std::this_thread::sleep_for(1ms);
    }
    auto elapsed2 = std::chrono::steady_clock::now() - start2;
    const auto measurement2 = measureThroughput(message_count, elapsed2);
    scheduler2.stop();

    // 输出对比结果
    LogInfo("");
    LogInfo("| Channel Type   | Time (ms) | Throughput (msg/s) |");
    LogInfo("|----------------|-----------|-------------------|");
    LogInfo("| galay::spsc::UnboundedChannel  | {:>9.3f} | {:>17.0f} |",
            measurement1.elapsed_ms, measurement1.throughput);
    LogInfo("| galay::mpsc::UnboundedChannel    | {:>9.3f} | {:>17.0f} |",
            measurement2.elapsed_ms, measurement2.throughput);
    LogInfo("");

    double speedup = measurement1.throughput / measurement2.throughput;
    LogInfo("galay::spsc::UnboundedChannel is {:.2f}x {} than galay::mpsc::UnboundedChannel (same scheduler)",
            speedup > 1 ? speedup : 1/speedup,
            speedup > 1 ? "faster" : "slower");
}

int main() {
    LogInfo("=== galay::spsc::UnboundedChannel Benchmark ===");
    LogInfo("role: same-thread / same-scheduler high-performance channel");
    LogInfo("note: galay::mpsc::UnboundedChannel numbers in this benchmark are reference-only because semantics differ");
    LogInfo("");

    // 1. galay::spsc::UnboundedChannel 吞吐量
    benchUnsafeChannelThroughput(THROUGHPUT_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) {
        return 1;
    }
    LogInfo("");

    // 2. galay::spsc::UnboundedChannel 批量接收吞吐量
    benchUnsafeChannelBatchThroughput(THROUGHPUT_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) {
        return 1;
    }
    LogInfo("");

    // 2b. galay::spsc::UnboundedChannel recvBatched 攒批接收吞吐量（不同 limit）
    benchUnsafeChannelBatchedThroughput(THROUGHPUT_MESSAGES, 100);
    if (g_benchmark_failed.load(std::memory_order_acquire)) {
        return 1;
    }
    LogInfo("");
    benchUnsafeChannelBatchedThroughput(THROUGHPUT_MESSAGES, 500);
    if (g_benchmark_failed.load(std::memory_order_acquire)) {
        return 1;
    }
    LogInfo("");
    benchUnsafeChannelBatchedThroughput(THROUGHPUT_MESSAGES, 1000);
    if (g_benchmark_failed.load(std::memory_order_acquire)) {
        return 1;
    }
    LogInfo("");

    // 3. galay::spsc::UnboundedChannel 延迟测试
    benchUnsafeChannelLatency(LATENCY_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) {
        return 1;
    }
    LogInfo("");

    // 4. galay::mpsc::UnboundedChannel 吞吐量（对比）
    benchMpscChannelThroughput(THROUGHPUT_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) {
        return 1;
    }
    LogInfo("");

    // 5. 性能对比
    benchComparison(THROUGHPUT_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) {
        return 1;
    }
    LogInfo("");

    LogInfo("=== Benchmark Complete ===");

    return 0;
}
