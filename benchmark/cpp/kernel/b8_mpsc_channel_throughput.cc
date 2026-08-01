/**
 * @file b8_mpsc.cc
 * @brief 用途：压测 `galay::mpsc::UnboundedChannel` 在跨线程与跨运行时场景下的通信性能。
 * 关键覆盖点：单/多生产者吞吐、批量接收、延迟采样、跨 runtime 场景与正确性统计。
 * 通过条件：所有压测阶段完成并输出吞吐或延迟结果，进程无崩溃、卡死或超时。
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>
#include <set>
#include <mutex>
#include <numeric>
#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/compute_scheduler.h>
#include "test/cpp/common/stdout_log.h"

using namespace galay::kernel;
using namespace std::chrono_literals;

// ============== 压测参数 ==============
[[maybe_unused]] constexpr int WARMUP_COUNT = 10000;
constexpr int THROUGHPUT_MESSAGES = 1000000;
constexpr int LATENCY_MESSAGES = 100000;
constexpr int CORRECTNESS_MESSAGES = 100000;
constexpr std::size_t PRODUCER_THROUGHPUT_SAMPLE_COUNT = 5;
constexpr auto PRODUCER_MIN_SAMPLE_DURATION = 300ms;
constexpr std::size_t BATCH_SAMPLE_COUNT = 5;
constexpr auto BATCH_MIN_SAMPLE_DURATION = 300ms;
constexpr std::size_t LATENCY_SAMPLE_COUNT = 5;
constexpr std::size_t CROSS_SCHEDULER_SAMPLE_COUNT = 5;
constexpr auto CORRECTNESS_WAIT_TIMEOUT = 10s;

// ============== 全局计数器 ==============
std::atomic<int64_t> g_sent{0};
std::atomic<int64_t> g_received{0};
std::atomic<int64_t> g_sum{0};
std::atomic<int64_t> g_latency_sum_ns{0};
std::atomic<int64_t> g_latency_count{0};
std::atomic<bool> g_consumer_done{false};
std::atomic<bool> g_consumer_ready{false};
std::atomic<bool> g_benchmark_failed{false};
galay::benchmark::CompletionLatch* g_consumer_done_latch = nullptr;

// 正确性验证
std::mutex g_received_mutex;
std::set<int64_t> g_received_set;

void resetCounters() {
    g_sent = 0;
    g_received = 0;
    g_sum = 0;
    g_latency_sum_ns = 0;
    g_latency_count = 0;
    g_consumer_done = false;
    g_consumer_ready = false;
    g_consumer_done_latch = nullptr;
    g_received_set.clear();
}

// ============== 消息结构 ==============
struct TimestampedMessage {
    int64_t id;
    std::chrono::steady_clock::time_point send_time;
};

struct ThroughputSample {
    double elapsed_ms;
    double throughput;
    int64_t received;
    int64_t sum;
};

struct LatencySample {
    double avg_latency_us;
    int64_t received;
};

// ============== 消费者协程 ==============

// 简单消费者（吞吐量测试）
Task<void> simpleConsumer(galay::mpsc::UnboundedChannel<int64_t>* channel, int64_t expected_count) {
    g_consumer_ready.store(true, std::memory_order_release);
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        auto value = co_await channel->recv();
        if (!value) {
            if (IOError::contains(value.error().code(), kTimeout)) {
                continue;
            }
            g_benchmark_failed.store(true, std::memory_order_release);
            break;
        }
        ++received;
        sum += *value;
    }
    g_received.store(received, std::memory_order_relaxed);
    g_sum.store(sum, std::memory_order_relaxed);
    g_consumer_done = true;
    if (g_consumer_done_latch) {
        g_consumer_done_latch->arrive();
    }
    co_return;
}

// 批量消费者
Task<void> batchConsumer(galay::mpsc::UnboundedChannel<int64_t>* channel, int64_t expected_count) {
    g_consumer_ready.store(true, std::memory_order_release);
    int64_t received = 0;
    int64_t sum = 0;
    while (received < expected_count) {
        auto batch = co_await channel->recvBatch(256);
        if (!batch) {
            if (IOError::contains(batch.error().code(), kTimeout)) {
                continue;
            }
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
    if (g_consumer_done_latch) {
        g_consumer_done_latch->arrive();
    }
    co_return;
}

// 延迟测试消费者
Task<void> latencyConsumer(galay::mpsc::UnboundedChannel<TimestampedMessage>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t latency_sum_ns = 0;
    g_consumer_ready.store(true, std::memory_order_release);
    while (received < expected_count) {
        auto msg = co_await channel->recv();
        if (!msg) {
            if (IOError::contains(msg.error().code(), kTimeout)) {
                continue;
            }
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
    if (g_consumer_done_latch) {
        g_consumer_done_latch->arrive();
    }
    co_return;
}

// 正确性验证消费者
Task<void> correctnessConsumer(galay::mpsc::UnboundedChannel<int64_t>* channel, int64_t expected_count) {
    while (g_received < expected_count) {
        auto value = co_await channel->recv();
        if (!value) {
            if (IOError::contains(value.error().code(), kTimeout)) {
                continue;
            }
            g_benchmark_failed.store(true, std::memory_order_release);
            break;
        }
        {
            std::lock_guard<std::mutex> lock(g_received_mutex);
            g_received_set.insert(*value);
        }
        g_received.fetch_add(1, std::memory_order_relaxed);
    }
    g_consumer_done = true;
    co_return;
}

// ============== 生产者函数 ==============

// 单线程生产者
void singleProducer(galay::mpsc::UnboundedChannel<int64_t>* channel, int64_t count) {
    int64_t sent = 0;
    for (int64_t i = 0; i < count; ++i) {
        if (!channel->send(i)) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  single producer send failed after {} messages", sent);
            if (!channel->close() && !channel->isClosed()) {
                LogError("  channel close failed after single producer send failure");
            }
            break;
        }
        ++sent;
    }
    g_sent.store(sent, std::memory_order_relaxed);
}

// 多线程生产者
void multiProducer(galay::mpsc::UnboundedChannel<int64_t>* channel, int64_t start, int64_t count) {
    int64_t sent = 0;
    for (int64_t i = 0; i < count; ++i) {
        if (!channel->send(start + i)) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  producer starting at {} send failed after {} messages", start, sent);
            if (!channel->close() && !channel->isClosed()) {
                LogError("  channel close failed after multi-producer send failure");
            }
            break;
        }
        ++sent;
    }
    g_sent.fetch_add(sent, std::memory_order_relaxed);
}

// 延迟测试生产者
void latencyProducer(galay::mpsc::UnboundedChannel<TimestampedMessage>* channel, int64_t count) {
    int64_t sent = 0;
    for (int64_t i = 0; i < count; ++i) {
        TimestampedMessage msg;
        msg.id = i;
        msg.send_time = std::chrono::steady_clock::now();
        if (!channel->send(std::move(msg))) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  latency producer send failed after {} messages", sent);
            if (!channel->close() && !channel->isClosed()) {
                LogError("  channel close failed after latency send failure");
            }
            break;
        }
        ++sent;
    }
    g_sent.store(sent, std::memory_order_relaxed);
}

Task<void> crossSchedulerProducer(galay::mpsc::UnboundedChannel<int64_t>* channel,
                                  int64_t count,
                                  galay::benchmark::CompletionLatch* completion_latch) {
    int64_t sent = 0;
    for (int64_t i = 0; i < count; ++i) {
        if (!channel->send(i)) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  cross-scheduler producer send failed after {} messages", sent);
            if (!channel->close() && !channel->isClosed()) {
                LogError("  channel close failed after cross-scheduler send failure");
            }
            break;
        }
        ++sent;
        if (i != 0 && (i % 100) == 0) {
            co_yield true;
        }
    }
    g_sent.store(sent, std::memory_order_relaxed);
    if (completion_latch) {
        completion_latch->arrive();
    }
    co_return;
}

// ============== 压测函数 ==============

// 1. 单生产者吞吐量测试
void benchSingleProducerThroughput(int64_t message_count) {
    LogInfo("--- Single Producer Throughput Test ({} messages) ---", message_count);
    std::vector<ThroughputSample> samples;
    samples.reserve(PRODUCER_THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < PRODUCER_THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        int64_t sample_message_count = message_count;
        while (true) {
            resetCounters();

            galay::mpsc::UnboundedChannel<int64_t> channel;
            ComputeScheduler scheduler;

            auto started = scheduler.start();
            if (!started) {
                g_benchmark_failed.store(true, std::memory_order_release);
                LogError("  failed to start single-producer scheduler");
                return;
            }
            galay::benchmark::CompletionLatch consumer_done_latch(1);
            g_consumer_done_latch = &consumer_done_latch;

            if (!scheduleTask(scheduler, simpleConsumer(&channel, sample_message_count))) {
                g_benchmark_failed.store(true, std::memory_order_release);
                LogError("  failed to schedule single-producer consumer");
                g_consumer_done_latch = nullptr;
                scheduler.stop();
                return;
            }
            if (!galay::benchmark::waitForFlag(g_consumer_ready, 2s)) {
                LogError("  consumer did not become ready before throughput run");
                g_consumer_done_latch = nullptr;
                scheduler.stop();
                return;
            }

            galay::benchmark::CompletionLatch producer_ready_latch(1);
            galay::benchmark::StartGate start_gate;
            std::thread producer([&]() {
                producer_ready_latch.arrive();
                start_gate.wait();
                singleProducer(&channel, sample_message_count);
            });

            if (!producer_ready_latch.waitFor(2s)) {
                LogError("  producer did not become ready before throughput run");
                start_gate.open();
                producer.join();
                g_consumer_done_latch = nullptr;
                scheduler.stop();
                return;
            }

            const auto start = std::chrono::steady_clock::now();
            start_gate.open();
            consumer_done_latch.wait();

            const auto elapsed = std::chrono::steady_clock::now() - start;
            const auto elapsed_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

            producer.join();
            g_consumer_done_latch = nullptr;
            scheduler.stop();

            if (g_benchmark_failed.load(std::memory_order_acquire)) {
                LogError("  single-producer throughput sample failed");
                return;
            }

            if (elapsed < PRODUCER_MIN_SAMPLE_DURATION) {
                sample_message_count *= 2;
                continue;
            }

            samples.push_back({
                .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
                .throughput = elapsed_ns > 0
                    ? (static_cast<double>(sample_message_count) * 1'000'000'000.0 / elapsed_ns)
                    : 0.0,
                .received = g_received.load(std::memory_order_relaxed),
                .sum = g_sum.load(std::memory_order_relaxed),
            });
            break;
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

// 2. 多生产者吞吐量测试
void benchMultiProducerThroughput(int producer_count, int64_t total_messages) {
    LogInfo("--- Multi Producer Throughput Test ({} producers, {} messages) ---",
            producer_count, total_messages);
    std::vector<ThroughputSample> samples;
    samples.reserve(PRODUCER_THROUGHPUT_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < PRODUCER_THROUGHPUT_SAMPLE_COUNT;
         ++sample_index) {
        resetCounters();

        galay::mpsc::UnboundedChannel<int64_t> channel;
        ComputeScheduler scheduler;

        auto started = scheduler.start();
        if (!started) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  failed to start multi-producer scheduler");
            return;
        }
        galay::benchmark::CompletionLatch consumer_done_latch(1);
        g_consumer_done_latch = &consumer_done_latch;
        if (!scheduleTask(scheduler, simpleConsumer(&channel, total_messages))) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  failed to schedule multi-producer consumer");
            g_consumer_done_latch = nullptr;
            scheduler.stop();
            return;
        }
        if (!galay::benchmark::waitForFlag(g_consumer_ready, 2s)) {
            LogError("  consumer did not become ready before multi-producer run");
            g_consumer_done_latch = nullptr;
            scheduler.stop();
            return;
        }

        const int64_t per_producer = total_messages / producer_count;
        const auto start = std::chrono::steady_clock::now();

        std::vector<std::thread> producers;
        for (int i = 0; i < producer_count; ++i) {
            const int64_t start_id = i * per_producer;
            producers.emplace_back(multiProducer, &channel, start_id, per_producer);
        }

        for (auto& t : producers) {
            t.join();
        }

        consumer_done_latch.wait();

        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

        g_consumer_done_latch = nullptr;
        scheduler.stop();

        if (g_benchmark_failed.load(std::memory_order_acquire)) {
            LogError("  multi-producer throughput sample failed");
            return;
        }

        samples.push_back({
            .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
            .throughput = elapsed_ns > 0
                ? (static_cast<double>(total_messages) * 1'000'000'000.0 / elapsed_ns)
                : 0.0,
            .received = g_received.load(std::memory_order_relaxed),
            .sum = g_sum.load(std::memory_order_relaxed),
        });
    }

    const auto median_sample = galay::benchmark::medianElement(
        std::move(samples),
        [](const ThroughputSample& lhs, const ThroughputSample& rhs) {
            return lhs.throughput < rhs.throughput;
        });

    LogInfo("  sent={}, received={}, time={}ms, throughput={:.0f} msg/s",
            total_messages,
            median_sample.received,
            median_sample.elapsed_ms,
            median_sample.throughput);
}

// 3. 批量接收吞吐量测试
void benchBatchReceiveThroughput(int64_t message_count) {
    LogInfo("--- Batch Receive Throughput Test ({} messages) ---", message_count);
    std::vector<ThroughputSample> samples;
    samples.reserve(BATCH_SAMPLE_COUNT);

    for (std::size_t sample_index = 0; sample_index < BATCH_SAMPLE_COUNT; ++sample_index) {
        int64_t sample_message_count = message_count;
        while (true) {
            resetCounters();

            galay::mpsc::UnboundedChannel<int64_t> channel;
            ComputeScheduler scheduler;

            auto started = scheduler.start();
            if (!started) {
                g_benchmark_failed.store(true, std::memory_order_release);
                LogError("  failed to start batch scheduler");
                return;
            }
            galay::benchmark::CompletionLatch consumer_done_latch(1);
            g_consumer_done_latch = &consumer_done_latch;
            if (!scheduleTask(scheduler, batchConsumer(&channel, sample_message_count))) {
                g_benchmark_failed.store(true, std::memory_order_release);
                LogError("  failed to schedule batch consumer");
                g_consumer_done_latch = nullptr;
                scheduler.stop();
                return;
            }
            if (!galay::benchmark::waitForFlag(g_consumer_ready, 2s)) {
                LogError("  consumer did not become ready before batch run");
                g_consumer_done_latch = nullptr;
                scheduler.stop();
                return;
            }

            const auto start = std::chrono::steady_clock::now();
            std::thread producer([&]() {
                singleProducer(&channel, sample_message_count);
            });

            consumer_done_latch.wait();

            const auto elapsed = std::chrono::steady_clock::now() - start;
            const auto elapsed_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

            producer.join();
            g_consumer_done_latch = nullptr;
            scheduler.stop();

            if (g_benchmark_failed.load(std::memory_order_acquire)) {
                LogError("  batch throughput sample failed");
                return;
            }

            if (elapsed < BATCH_MIN_SAMPLE_DURATION) {
                sample_message_count *= 2;
                continue;
            }

            samples.push_back({
                .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
                .throughput = elapsed_ns > 0
                    ? (static_cast<double>(sample_message_count) * 1'000'000'000.0 / elapsed_ns)
                    : 0.0,
                .received = g_received.load(std::memory_order_relaxed),
                .sum = g_sum.load(std::memory_order_relaxed),
            });
            break;
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

// 4. 延迟测试
void benchLatency(int64_t message_count) {
    LogInfo("--- Latency Test ({} messages) ---", message_count);
    std::vector<LatencySample> samples;
    samples.reserve(LATENCY_SAMPLE_COUNT);

    for (std::size_t sample_index = 0; sample_index < LATENCY_SAMPLE_COUNT; ++sample_index) {
        resetCounters();

        galay::mpsc::UnboundedChannel<TimestampedMessage> channel;
        ComputeScheduler scheduler;

        auto started = scheduler.start();
        if (!started) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  failed to start latency scheduler");
            return;
        }
        galay::benchmark::CompletionLatch consumer_done_latch(1);
        g_consumer_done_latch = &consumer_done_latch;
        if (!scheduleTask(scheduler, latencyConsumer(&channel, message_count))) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  failed to schedule latency consumer");
            g_consumer_done_latch = nullptr;
            scheduler.stop();
            return;
        }

        if (!galay::benchmark::waitForFlag(g_consumer_ready, 2s)) {
            LogError("  consumer did not become ready before latency run");
            g_consumer_done_latch = nullptr;
            scheduler.stop();
            return;
        }

        std::thread producer([&]() {
            latencyProducer(&channel, message_count);
        });

        consumer_done_latch.wait();

        producer.join();
        g_consumer_done_latch = nullptr;
        scheduler.stop();

        if (g_benchmark_failed.load(std::memory_order_acquire)) {
            LogError("  latency sample failed");
            return;
        }

        samples.push_back({
            .avg_latency_us = (double)g_latency_sum_ns.load(std::memory_order_relaxed) /
                g_latency_count.load(std::memory_order_relaxed) / 1000.0,
            .received = g_received.load(std::memory_order_relaxed),
        });
    }

    const auto median_sample = galay::benchmark::medianElement(
        std::move(samples),
        [](const LatencySample& lhs, const LatencySample& rhs) {
            return lhs.avg_latency_us < rhs.avg_latency_us;
        });

    LogInfo("  messages={}, avg_latency={:.2f}us", median_sample.received, median_sample.avg_latency_us);
}

// 5. 正确性验证测试
void benchCorrectness(int producer_count, int64_t total_messages) {
    LogInfo("--- Correctness Test ({} producers, {} messages) ---",
            producer_count, total_messages);
    resetCounters();

    galay::mpsc::UnboundedChannel<int64_t> channel;
    ComputeScheduler scheduler;

    auto started = scheduler.start();
    if (!started) {
        g_benchmark_failed.store(true, std::memory_order_release);
        LogError("  failed to start correctness scheduler");
        return;
    }
    if (!scheduleTask(scheduler, correctnessConsumer(&channel, total_messages))) {
        g_benchmark_failed.store(true, std::memory_order_release);
        LogError("  failed to schedule correctness consumer");
        scheduler.stop();
        return;
    }

    int64_t per_producer = total_messages / producer_count;

    // 启动多个生产者线程
    std::vector<std::thread> producers;
    for (int i = 0; i < producer_count; ++i) {
        int64_t start_id = i * per_producer;
        producers.emplace_back(multiProducer, &channel, start_id, per_producer);
    }

    for (auto& t : producers) {
        t.join();
    }

    if (!galay::benchmark::waitForFlag(g_consumer_done, CORRECTNESS_WAIT_TIMEOUT, 1ms)) {
        LogError("  correctness consumer timed out after {}s", CORRECTNESS_WAIT_TIMEOUT.count());
    }

    scheduler.stop();

    // 验证正确性
    bool no_loss = (g_received_set.size() == (size_t)total_messages);
    bool no_duplicate = (g_received == total_messages);

    // 检查是否所有消息都收到
    std::set<int64_t> expected_set;
    for (int i = 0; i < producer_count; ++i) {
        int64_t start_id = i * per_producer;
        for (int64_t j = 0; j < per_producer; ++j) {
            expected_set.insert(start_id + j);
        }
    }

    bool all_received = (g_received_set == expected_set);

    LogInfo("  sent={}, received={}, unique={}",
            g_sent.load(), g_received.load(), g_received_set.size());
    LogInfo("  no_loss={}, no_duplicate={}, all_correct={}",
            no_loss ? "YES" : "NO",
            no_duplicate ? "YES" : "NO",
            all_received ? "YES" : "NO");

    if (!all_received) {
        LogError("  CORRECTNESS FAILED!");
    }
}

// 6. 跨调度器测试
void benchCrossScheduler(int64_t message_count) {
    LogInfo("--- Cross-Scheduler Test ({} messages) ---", message_count);
    std::vector<ThroughputSample> samples;
    samples.reserve(CROSS_SCHEDULER_SAMPLE_COUNT);

    for (std::size_t sample_index = 0;
         sample_index < CROSS_SCHEDULER_SAMPLE_COUNT;
         ++sample_index) {
        resetCounters();

        galay::mpsc::UnboundedChannel<int64_t> channel;
        ComputeScheduler consumerScheduler;

        auto consumer_started = consumerScheduler.start();
        if (!consumer_started) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  failed to start cross-scheduler consumer scheduler");
            return;
        }
        galay::benchmark::CompletionLatch consumer_done_latch(1);
        g_consumer_done_latch = &consumer_done_latch;
        if (!scheduleTask(consumerScheduler, simpleConsumer(&channel, message_count))) {
            g_benchmark_failed.store(true, std::memory_order_release);
            LogError("  failed to schedule cross-scheduler consumer");
            g_consumer_done_latch = nullptr;
            consumerScheduler.stop();
            return;
        }
        if (!galay::benchmark::waitForFlag(g_consumer_ready, 2s)) {
            LogError("  consumer did not become ready before cross-scheduler run");
            g_consumer_done_latch = nullptr;
            consumerScheduler.stop();
            return;
        }

        galay::benchmark::CompletionLatch producer_ready_latch(1);
        galay::benchmark::CompletionLatch producer_done_latch(1);
        galay::benchmark::StartGate start_gate;

        std::thread producer_thread([&]() {
            ComputeScheduler producerScheduler;
            auto producer_started = producerScheduler.start();
            if (!producer_started) {
                g_benchmark_failed.store(true, std::memory_order_release);
                LogError("  failed to start cross-scheduler producer scheduler");
                producer_ready_latch.arrive();
                if (!channel.close() && !channel.isClosed()) {
                    LogError("  channel close failed after producer scheduler start failure");
                }
                producer_done_latch.arrive();
                return;
            }
            producer_ready_latch.arrive();
            start_gate.wait();
            if (!scheduleTask(
                    producerScheduler,
                    crossSchedulerProducer(&channel, message_count, &producer_done_latch))) {
                g_benchmark_failed.store(true, std::memory_order_release);
                LogError("  failed to schedule cross-scheduler producer");
                if (!channel.close() && !channel.isClosed()) {
                    LogError("  channel close failed after producer scheduling failure");
                }
                producer_done_latch.arrive();
                producerScheduler.stop();
                return;
            }
            producer_done_latch.wait();
            producerScheduler.stop();
        });

        if (!producer_ready_latch.waitFor(2s)) {
            LogError("  producer did not become ready before cross-scheduler run");
            start_gate.open();
            producer_thread.join();
            g_consumer_done_latch = nullptr;
            consumerScheduler.stop();
            return;
        }

        const auto start = std::chrono::steady_clock::now();
        start_gate.open();
        consumer_done_latch.wait();
        producer_done_latch.wait();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

        producer_thread.join();
        g_consumer_done_latch = nullptr;
        consumerScheduler.stop();

        if (g_benchmark_failed.load(std::memory_order_acquire)) {
            LogError("  cross-scheduler sample failed");
            return;
        }

        samples.push_back({
            .elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0,
            .throughput = elapsed_ns > 0
                ? (static_cast<double>(message_count) * 1'000'000'000.0 / elapsed_ns)
                : 0.0,
            .received = g_received.load(std::memory_order_relaxed),
            .sum = g_sum.load(std::memory_order_relaxed),
        });
    }

    const auto median_sample = galay::benchmark::medianElement(
        std::move(samples),
        [](const ThroughputSample& lhs, const ThroughputSample& rhs) {
            return lhs.throughput < rhs.throughput;
        });
    const int64_t expected_sum = (message_count - 1) * message_count / 2;
    const bool correct =
        (median_sample.received == message_count) && (median_sample.sum == expected_sum);

    LogInfo("  sent={}, received={}, time={}ms, throughput={:.0f} msg/s",
            message_count,
            median_sample.received,
            median_sample.elapsed_ms,
            median_sample.throughput);
    LogInfo("  sum={} (expected {}), correct={}",
            median_sample.sum, expected_sum, correct ? "YES" : "NO");
}

// 7. 持续压力测试
void benchSustained(int duration_sec) {
    LogInfo("--- Sustained Load Test ({}s) ---", duration_sec);
    resetCounters();

    galay::mpsc::UnboundedChannel<int64_t> channel;
    ComputeScheduler scheduler;

    std::atomic<bool> running{true};

    // 消费者协程
    auto sustainedConsumer = [](galay::mpsc::UnboundedChannel<int64_t>* ch) -> Task<void> {
        for (;;) {
            auto value = co_await ch->recv();
            if (!value) {
                if (IOError::contains(value.error().code(), kTimeout)) {
                    continue;
                }
                if (IOError::contains(value.error().code(), kClosed)) {
                    break;
                }
                g_benchmark_failed.store(true, std::memory_order_release);
                break;
            }
            g_received.fetch_add(1, std::memory_order_relaxed);
        }
        g_consumer_done = true;
        co_return;
    };

    auto started = scheduler.start();
    if (!started) {
        g_benchmark_failed.store(true, std::memory_order_release);
        LogError("  failed to start sustained scheduler");
        return;
    }
    if (!scheduleTask(scheduler, sustainedConsumer(&channel))) {
        g_benchmark_failed.store(true, std::memory_order_release);
        LogError("  failed to schedule sustained consumer");
        scheduler.stop();
        return;
    }

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    // 生产者线程
    std::vector<std::thread> producers(4);
    for(auto& producer: producers) {
        producer = std::thread([&]() {
            int64_t id = 0;
            while (running.load(std::memory_order_acquire)) {
                if (!channel.send(id)) {
                    g_benchmark_failed.store(true, std::memory_order_release);
                    running.store(false, std::memory_order_release);
                    LogError("  sustained producer send failed after {} messages", id);
                    if (!channel.close() && !channel.isClosed()) {
                        LogError("  channel close failed after sustained send failure");
                    }
                    break;
                }
                ++id;
                g_sent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 监控
    int64_t last_received = 0;
    int64_t recent_receive_rate = 0;
    while (std::chrono::steady_clock::now() < end_time) {
        std::this_thread::sleep_for(1s);
        int64_t current = g_received.load();
        recent_receive_rate = current - last_received;
        LogInfo("  throughput: {}/s, total sent: {}, received: {}",
                recent_receive_rate, g_sent.load(), current);
        last_received = current;
    }

    running = false;
    for(auto& producer: producers) {
        producer.join();
    }

    if (!channel.close() && !channel.isClosed()) {
        g_benchmark_failed.store(true, std::memory_order_release);
        LogError("  failed to close sustained channel after producers stopped");
    }

    // 根据实测消费速率给积压留出排空时间，且保留总超时避免 benchmark 卡死。
    const int64_t sent = g_sent.load(std::memory_order_relaxed);
    const int64_t received = g_received.load(std::memory_order_relaxed);
    const int64_t pending = std::max<int64_t>(sent - received, 0);
    const int64_t receive_rate = std::max<int64_t>(recent_receive_rate, 1);
    const int64_t estimated_seconds =
        pending / receive_rate + (pending % receive_rate != 0 ? 1 : 0);
    const int64_t drain_timeout_seconds = std::max<int64_t>(
        10, std::min<int64_t>(estimated_seconds, 55) * 2 + 10);
    const auto drain_timeout = std::chrono::seconds(drain_timeout_seconds);
    if (!galay::benchmark::waitForFlag(g_consumer_done, drain_timeout, 10ms)) {
        g_benchmark_failed.store(true, std::memory_order_release);
        LogError("  consumer drain timed out after {}s with {} queued messages remaining",
                 drain_timeout.count(),
                 channel.size());
    }

    scheduler.stop();

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    double avg_throughput = (double)g_received / ms * 1000.0;

    if (g_sent.load(std::memory_order_relaxed) !=
        g_received.load(std::memory_order_relaxed)) {
        g_benchmark_failed.store(true, std::memory_order_release);
        LogError("  sustained benchmark lost messages: sent={}, received={}",
                 g_sent.load(std::memory_order_relaxed),
                 g_received.load(std::memory_order_relaxed));
    }

    LogInfo("  total: sent={}, received={}, avg throughput: {:.0f}/s",
            g_sent.load(), g_received.load(), avg_throughput);
}

int main() {
    LogInfo("=== galay::mpsc::UnboundedChannel Benchmark ===");
    LogInfo("role: cross-thread MPSC channel, single-consumer correctness path");
    LogInfo("note: use B9-galay::spsc::UnboundedChannel for same-thread/high-performance channel measurements");
    LogInfo("");

    // 1. 单生产者吞吐量
    benchSingleProducerThroughput(THROUGHPUT_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    // 2. 多生产者吞吐量
    benchMultiProducerThroughput(4, THROUGHPUT_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    // 3. 批量接收吞吐量
    benchBatchReceiveThroughput(THROUGHPUT_MESSAGES * 5);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    // 4. 延迟测试
    benchLatency(LATENCY_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    // 5. 正确性验证
    benchCorrectness(4, CORRECTNESS_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    // 6. 跨调度器测试
    benchCrossScheduler(THROUGHPUT_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    // 7. 持续压力测试
    benchSustained(5);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    LogInfo("=== Benchmark Complete ===");

    return 0;
}
