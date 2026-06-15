/**
 * @file b9_unsafe.cc
 * @brief 用途：压测 `UnsafeChannel` 在同线程协程通信场景下的极限性能。
 * 关键覆盖点：同调度器吞吐、延迟采样、与 `MpscChannel` 的参考量级对照。
 * 通过条件：所有测量样本完成并输出统计结果，进程无异常退出。
 */

#include <atomic>
#include <chrono>
#include <vector>
#include "benchmark/benchmark_sync.h"
#include "galay-kernel/concurrency/unsafe_channel.h"
#include "galay-kernel/concurrency/mpsc_channel.h"
#include "galay-kernel/core/task.h"
#include "galay-kernel/core/compute_scheduler.h"
#include "test/stdout_log.h"

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

void resetCounters() {
    g_sent = 0;
    g_received = 0;
    g_sum = 0;
    g_latency_sum_ns = 0;
    g_latency_count = 0;
    g_consumer_done = false;
    g_producer_done = false;
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
        if (elapsed >= THROUGHPUT_MIN_SAMPLE_DURATION) {
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

// ============== UnsafeChannel 消费者协程 ==============

Task<void> unsafeSimpleConsumer(UnsafeChannel<int64_t>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        auto value = co_await channel->recv();
        if (value) {
            ++received;
            sum += *value;
        }
    }
    g_received.store(received, std::memory_order_relaxed);
    g_sum.store(sum, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

Task<void> unsafeBatchConsumer(UnsafeChannel<int64_t>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        auto batch = co_await channel->recvBatch(256);
        if (batch) {
            for (int64_t v : *batch) {
                sum += v;
            }
            received += batch->size();
        }
    }
    g_received.store(received, std::memory_order_relaxed);
    g_sum.store(sum, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

Task<void> unsafeBatchedConsumer(UnsafeChannel<int64_t>* channel, int64_t expected_count, int64_t batch_limit) {
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        // 使用 recvBatched 攒批接收，带超时
        auto batch = co_await channel->recvBatched(batch_limit).timeout(10ms);
        if (batch) {
            for (int64_t v : *batch) {
                sum += v;
            }
            received += batch->size();
        }
    }
    g_received.store(received, std::memory_order_relaxed);
    g_sum.store(sum, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

Task<void> unsafeLatencyConsumer(UnsafeChannel<TimestampedMessage>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t latency_sum_ns = 0;
    while (received < expected_count) {
        auto msg = co_await channel->recv();
        if (msg) {
            auto now = std::chrono::steady_clock::now();
            auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - msg->send_time).count();
            latency_sum_ns += latency_ns;
            ++received;
        }
    }
    g_received.store(received, std::memory_order_relaxed);
    g_latency_sum_ns.store(latency_sum_ns, std::memory_order_relaxed);
    g_latency_count.store(received, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

// ============== UnsafeChannel 生产者协程 ==============

Task<void> unsafeSimpleProducer(UnsafeChannel<int64_t>* channel, int64_t count) {
    for (int64_t i = 0; i < count; ++i) {
        channel->send(i);
        if (i % 1000 == 0) {
            co_yield true;  // 让出执行权
        }
    }
    g_sent.store(count, std::memory_order_relaxed);
    g_producer_done = true;
    co_return;
}

Task<void> unsafeLatencyProducer(UnsafeChannel<TimestampedMessage>* channel, int64_t count) {
    for (int64_t i = 0; i < count; ++i) {
        TimestampedMessage msg;
        msg.id = i;
        msg.send_time = std::chrono::steady_clock::now();
        channel->send(std::move(msg));
        if (i % 100 == 0) {
            co_yield true;
        }
    }
    g_sent.store(count, std::memory_order_relaxed);
    g_producer_done = true;
    co_return;
}

// ============== MpscChannel 消费者协程（用于对比）==============

Task<void> mpscSimpleConsumer(MpscChannel<int64_t>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        auto value = co_await channel->recv();
        if (value) {
            ++received;
            sum += *value;
        }
    }
    g_received.store(received, std::memory_order_relaxed);
    g_sum.store(sum, std::memory_order_relaxed);
    g_consumer_done = true;
    co_return;
}

// ============== MpscChannel 生产者协程（用于对比）==============

Task<void> mpscSimpleProducer(MpscChannel<int64_t>* channel, int64_t count) {
    for (int64_t i = 0; i < count; ++i) {
        channel->send(i);
        if (i % 1000 == 0) {
            co_yield true;
        }
    }
    g_sent.store(count, std::memory_order_relaxed);
    g_producer_done = true;
    co_return;
}

// ============== 压测函数 ==============

// 1. UnsafeChannel 单生产者吞吐量测试
void benchUnsafeChannelThroughput(int64_t message_count) {
    LogInfo("--- UnsafeChannel Throughput Test ({} messages) ---", message_count);
    std::vector<ThroughputSample> samples;
    samples.reserve(THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        samples.push_back(measureThroughputSample(message_count, [&](int64_t sample_message_count) {
            resetCounters();

            UnsafeChannel<int64_t> channel;
            ComputeScheduler scheduler;

            scheduler.start();

            const auto start = std::chrono::steady_clock::now();

            scheduleTask(scheduler, unsafeSimpleConsumer(&channel, sample_message_count));
            scheduleTask(scheduler, unsafeSimpleProducer(&channel, sample_message_count));

            while (!g_consumer_done) {
                std::this_thread::sleep_for(1ms);
            }

            const auto elapsed = std::chrono::steady_clock::now() - start;
            scheduler.stop();
            return elapsed;
        }));
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

// 2. UnsafeChannel 批量接收吞吐量测试
void benchUnsafeChannelBatchThroughput(int64_t message_count) {
    LogInfo("--- UnsafeChannel Batch Receive Throughput Test ({} messages) ---", message_count);
    std::vector<ThroughputSample> samples;
    samples.reserve(THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        samples.push_back(measureThroughputSample(message_count, [&](int64_t sample_message_count) {
            resetCounters();

            UnsafeChannel<int64_t> channel;
            ComputeScheduler scheduler;

            scheduler.start();

            const auto start = std::chrono::steady_clock::now();

            scheduleTask(scheduler, unsafeBatchConsumer(&channel, sample_message_count));
            scheduleTask(scheduler, unsafeSimpleProducer(&channel, sample_message_count));

            while (!g_consumer_done) {
                std::this_thread::sleep_for(1ms);
            }

            const auto elapsed = std::chrono::steady_clock::now() - start;
            scheduler.stop();
            return elapsed;
        }));
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

// 2b. UnsafeChannel recvBatched 攒批接收吞吐量测试
void benchUnsafeChannelBatchedThroughput(int64_t message_count, int64_t batch_limit) {
    LogInfo("--- UnsafeChannel recvBatched Throughput Test ({} messages, limit={}) ---",
            message_count, batch_limit);
    std::vector<ThroughputSample> samples;
    samples.reserve(THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        samples.push_back(measureThroughputSample(message_count, [&](int64_t sample_message_count) {
            resetCounters();

            UnsafeChannel<int64_t> channel;
            ComputeScheduler scheduler;

            scheduler.start();

            const auto start = std::chrono::steady_clock::now();

            scheduleTask(scheduler, unsafeBatchedConsumer(&channel, sample_message_count, batch_limit));
            scheduleTask(scheduler, unsafeSimpleProducer(&channel, sample_message_count));

            while (!g_consumer_done) {
                std::this_thread::sleep_for(1ms);
            }

            const auto elapsed = std::chrono::steady_clock::now() - start;
            scheduler.stop();
            return elapsed;
        }));
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

// 3. UnsafeChannel 延迟测试
void benchUnsafeChannelLatency(int64_t message_count) {
    LogInfo("--- UnsafeChannel Latency Test ({} messages) ---", message_count);
    resetCounters();

    UnsafeChannel<TimestampedMessage> channel;
    ComputeScheduler scheduler;

    scheduler.start();

    scheduleTask(scheduler, unsafeLatencyConsumer(&channel, message_count));
    scheduleTask(scheduler, unsafeLatencyProducer(&channel, message_count));

    while (!g_consumer_done) {
        std::this_thread::sleep_for(1ms);
    }

    scheduler.stop();

    double avg_latency_us = (double)g_latency_sum_ns / g_latency_count / 1000.0;

    LogInfo("  messages={}, avg_latency={:.2f}us", g_received.load(), avg_latency_us);
}

// 4. MpscChannel 吞吐量测试（同调度器，用于对比）
void benchMpscChannelThroughput(int64_t message_count) {
    LogInfo("--- MpscChannel Throughput Test (same scheduler, {} messages) ---", message_count);
    std::vector<ThroughputSample> samples;
    samples.reserve(THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        samples.push_back(measureThroughputSample(message_count, [&](int64_t sample_message_count) {
            resetCounters();

            MpscChannel<int64_t> channel;
            ComputeScheduler scheduler;

            scheduler.start();

            const auto start = std::chrono::steady_clock::now();

            scheduleTask(scheduler, mpscSimpleConsumer(&channel, sample_message_count));
            scheduleTask(scheduler, mpscSimpleProducer(&channel, sample_message_count));

            while (!g_consumer_done) {
                std::this_thread::sleep_for(1ms);
            }

            const auto elapsed = std::chrono::steady_clock::now() - start;
            scheduler.stop();
            return elapsed;
        }));
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

    // UnsafeChannel 测试
    resetCounters();
    UnsafeChannel<int64_t> unsafeChannel;
    ComputeScheduler scheduler1;

    scheduler1.start();
    auto start1 = std::chrono::steady_clock::now();
    scheduleTask(scheduler1, unsafeSimpleConsumer(&unsafeChannel, message_count));
    scheduleTask(scheduler1, unsafeSimpleProducer(&unsafeChannel, message_count));
    while (!g_consumer_done) {
        std::this_thread::sleep_for(1ms);
    }
    auto elapsed1 = std::chrono::steady_clock::now() - start1;
    const auto measurement1 = measureThroughput(message_count, elapsed1);
    scheduler1.stop();

    // MpscChannel 测试
    resetCounters();
    MpscChannel<int64_t> mpscChannel;
    ComputeScheduler scheduler2;

    scheduler2.start();
    auto start2 = std::chrono::steady_clock::now();
    scheduleTask(scheduler2, mpscSimpleConsumer(&mpscChannel, message_count));
    scheduleTask(scheduler2, mpscSimpleProducer(&mpscChannel, message_count));
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
    LogInfo("| UnsafeChannel  | {:>9.3f} | {:>17.0f} |",
            measurement1.elapsed_ms, measurement1.throughput);
    LogInfo("| MpscChannel    | {:>9.3f} | {:>17.0f} |",
            measurement2.elapsed_ms, measurement2.throughput);
    LogInfo("");

    double speedup = measurement1.throughput / measurement2.throughput;
    LogInfo("UnsafeChannel is {:.2f}x {} than MpscChannel (same scheduler)",
            speedup > 1 ? speedup : 1/speedup,
            speedup > 1 ? "faster" : "slower");
}

int main() {
    LogInfo("=== UnsafeChannel Benchmark ===");
    LogInfo("role: same-thread / same-scheduler high-performance channel");
    LogInfo("note: MpscChannel numbers in this benchmark are reference-only because semantics differ");
    LogInfo("");

    // 1. UnsafeChannel 吞吐量
    benchUnsafeChannelThroughput(THROUGHPUT_MESSAGES);
    LogInfo("");

    // 2. UnsafeChannel 批量接收吞吐量
    benchUnsafeChannelBatchThroughput(THROUGHPUT_MESSAGES);
    LogInfo("");

    // 2b. UnsafeChannel recvBatched 攒批接收吞吐量（不同 limit）
    benchUnsafeChannelBatchedThroughput(THROUGHPUT_MESSAGES, 100);
    LogInfo("");
    benchUnsafeChannelBatchedThroughput(THROUGHPUT_MESSAGES, 500);
    LogInfo("");
    benchUnsafeChannelBatchedThroughput(THROUGHPUT_MESSAGES, 1000);
    LogInfo("");

    // 3. UnsafeChannel 延迟测试
    benchUnsafeChannelLatency(LATENCY_MESSAGES);
    LogInfo("");

    // 4. MpscChannel 吞吐量（对比）
    benchMpscChannelThroughput(THROUGHPUT_MESSAGES);
    LogInfo("");

    // 5. 性能对比
    benchComparison(THROUGHPUT_MESSAGES);
    LogInfo("");

    LogInfo("=== Benchmark Complete ===");

    return 0;
}
