/**
 * @file b8_mpsc.cc
 * @brief 用途：压测 `MpscChannel` 在跨线程与跨运行时场景下的通信性能。
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
#include "benchmark/benchmark_sync.h"
#include "galay-kernel/concurrency/mpsc_channel.h"
#include "galay-kernel/core/task.h"
#include "galay-kernel/core/compute_scheduler.h"
#include "test/stdout_log.h"

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
Task<void> simpleConsumer(MpscChannel<int64_t>* channel, int64_t expected_count) {
    g_consumer_ready.store(true, std::memory_order_release);
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
    if (g_consumer_done_latch) {
        g_consumer_done_latch->arrive();
    }
    co_return;
}

// 批量消费者
Task<void> batchConsumer(MpscChannel<int64_t>* channel, int64_t expected_count) {
    g_consumer_ready.store(true, std::memory_order_release);
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
    if (g_consumer_done_latch) {
        g_consumer_done_latch->arrive();
    }
    co_return;
}

// 延迟测试消费者
Task<void> latencyConsumer(MpscChannel<TimestampedMessage>* channel, int64_t expected_count) {
    int64_t received = 0;
    int64_t latency_sum_ns = 0;
    g_consumer_ready.store(true, std::memory_order_release);
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
    if (g_consumer_done_latch) {
        g_consumer_done_latch->arrive();
    }
    co_return;
}

// 正确性验证消费者
Task<void> correctnessConsumer(MpscChannel<int64_t>* channel, int64_t expected_count) {
    while (g_received < expected_count) {
        auto value = co_await channel->recv();
        if (value) {
            {
                std::lock_guard<std::mutex> lock(g_received_mutex);
                g_received_set.insert(*value);
            }
            g_received.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_consumer_done = true;
    co_return;
}

// ============== 生产者函数 ==============

// 单线程生产者
void singleProducer(MpscChannel<int64_t>* channel, int64_t count) {
    
    for (int64_t i = 0; i < count; ++i) {
        channel->send(i);
    }
    g_sent.store(count, std::memory_order_relaxed);
}

// 多线程生产者
void multiProducer(MpscChannel<int64_t>* channel, int64_t start, int64_t count) {
    
    for (int64_t i = 0; i < count; ++i) {
        channel->send(start + i);
    }
    g_sent.fetch_add(count, std::memory_order_relaxed);
}

// 延迟测试生产者
void latencyProducer(MpscChannel<TimestampedMessage>* channel, int64_t count) {

    for (int64_t i = 0; i < count; ++i) {
        TimestampedMessage msg;
        msg.id = i;
        msg.send_time = std::chrono::steady_clock::now();
        channel->send(std::move(msg));
    }
    g_sent.store(count, std::memory_order_relaxed);
}

Task<void> crossSchedulerProducer(MpscChannel<int64_t>* channel,
                                  int64_t count,
                                  galay::benchmark::CompletionLatch* completion_latch) {
    for (int64_t i = 0; i < count; ++i) {
        channel->send(i);
        if (i != 0 && (i % 100) == 0) {
            co_yield true;
        }
    }
    g_sent.store(count, std::memory_order_relaxed);
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

            MpscChannel<int64_t> channel;
            ComputeScheduler scheduler;

            scheduler.start();
            galay::benchmark::CompletionLatch consumer_done_latch(1);
            g_consumer_done_latch = &consumer_done_latch;

            scheduleTask(scheduler, simpleConsumer(&channel, sample_message_count));
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

        MpscChannel<int64_t> channel;
        ComputeScheduler scheduler;

        scheduler.start();
        galay::benchmark::CompletionLatch consumer_done_latch(1);
        g_consumer_done_latch = &consumer_done_latch;
        scheduleTask(scheduler, simpleConsumer(&channel, total_messages));
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

            MpscChannel<int64_t> channel;
            ComputeScheduler scheduler;

            scheduler.start();
            galay::benchmark::CompletionLatch consumer_done_latch(1);
            g_consumer_done_latch = &consumer_done_latch;
            scheduleTask(scheduler, batchConsumer(&channel, sample_message_count));
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

        MpscChannel<TimestampedMessage> channel;
        ComputeScheduler scheduler;

        scheduler.start();
        galay::benchmark::CompletionLatch consumer_done_latch(1);
        g_consumer_done_latch = &consumer_done_latch;
        scheduleTask(scheduler, latencyConsumer(&channel, message_count));

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

    MpscChannel<int64_t> channel;
    ComputeScheduler scheduler;

    scheduler.start();
    scheduleTask(scheduler, correctnessConsumer(&channel, total_messages));

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

        MpscChannel<int64_t> channel;
        ComputeScheduler consumerScheduler;

        consumerScheduler.start();
        galay::benchmark::CompletionLatch consumer_done_latch(1);
        g_consumer_done_latch = &consumer_done_latch;
        scheduleTask(consumerScheduler, simpleConsumer(&channel, message_count));
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
            producerScheduler.start();
            producer_ready_latch.arrive();
            start_gate.wait();
            scheduleTask(
                producerScheduler,
                crossSchedulerProducer(&channel, message_count, &producer_done_latch));
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

    MpscChannel<int64_t> channel;
    ComputeScheduler scheduler;

    std::atomic<bool> running{true};

    // 消费者协程
    auto sustainedConsumer = [](MpscChannel<int64_t>* ch, std::atomic<bool>* run) -> Task<void> {
        while (*run || ch->size() > 0) {
            auto value = co_await ch->recv();
            if (value) {
                g_received.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_consumer_done = true;
        co_return;
    };

    scheduler.start();
    scheduleTask(scheduler, sustainedConsumer(&channel, &running));

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    // 生产者线程
    std::vector<std::thread> producers(4);
    for(auto& producer: producers) {
        producer = std::thread([&]() {
            int64_t id = 0;
            while (running) {
                channel.send(id++);
                g_sent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 监控
    int64_t last_received = 0;
    while (std::chrono::steady_clock::now() < end_time) {
        std::this_thread::sleep_for(1s);
        int64_t current = g_received.load();
        int64_t delta = current - last_received;
        LogInfo("  throughput: {}/s, total sent: {}, received: {}",
                delta, g_sent.load(), current);
        last_received = current;
    }

    running = false;
    for(auto& producer: producers) {
        producer.join();
    }

    // 等待消费者处理完剩余消息
    const auto drain_timeout = std::chrono::seconds(std::max(duration_sec * 2, 10));
    if (!galay::benchmark::waitForFlag(g_consumer_done, drain_timeout, 10ms) && channel.size() > 0) {
        LogError("  consumer drain timed out after {}s with {} queued messages remaining",
                 drain_timeout.count(),
                 channel.size());
    }
    std::this_thread::sleep_for(100ms);

    scheduler.stop();

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    double avg_throughput = (double)g_received / ms * 1000.0;

    LogInfo("  total: sent={}, received={}, avg throughput: {:.0f}/s",
            g_sent.load(), g_received.load(), avg_throughput);
}

int main() {
    LogInfo("=== MpscChannel Benchmark ===");
    LogInfo("role: cross-thread MPSC channel, single-consumer correctness path");
    LogInfo("note: use B9-UnsafeChannel for same-thread/high-performance channel measurements");
    LogInfo("");

    // 1. 单生产者吞吐量
    benchSingleProducerThroughput(THROUGHPUT_MESSAGES);
    LogInfo("");

    // 2. 多生产者吞吐量
    benchMultiProducerThroughput(4, THROUGHPUT_MESSAGES);
    LogInfo("");

    // 3. 批量接收吞吐量
    benchBatchReceiveThroughput(THROUGHPUT_MESSAGES * 5);
    LogInfo("");

    // 4. 延迟测试
    benchLatency(LATENCY_MESSAGES);
    LogInfo("");

    // 5. 正确性验证
    benchCorrectness(4, CORRECTNESS_MESSAGES);
    LogInfo("");

    // 6. 跨调度器测试
    benchCrossScheduler(THROUGHPUT_MESSAGES);
    LogInfo("");

    // 7. 持续压力测试
    benchSustained(5);
    LogInfo("");

    LogInfo("=== Benchmark Complete ===");

    return 0;
}
