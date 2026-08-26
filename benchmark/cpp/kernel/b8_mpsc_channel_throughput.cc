/**
 * @file b8_mpsc.cc
 * @brief 用途：压测 `galay::mpsc::UnboundedChannel` 在多生产者场景下的通信性能。
 * 关键覆盖点：多生产者吞吐、正确性统计与持续压力。
 * 通过条件：所有压测阶段完成并输出吞吐结果，进程无崩溃、卡死或超时。
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>
#include <set>
#include <mutex>
#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include "test/cpp/common/stdout_log.h"

using namespace galay::kernel;
using namespace std::chrono_literals;

// ============== 压测参数 ==============
constexpr int THROUGHPUT_MESSAGES = 1000000;
constexpr int CORRECTNESS_MESSAGES = 100000;
constexpr std::size_t PRODUCER_THROUGHPUT_SAMPLE_COUNT = 5;
constexpr auto CORRECTNESS_WAIT_TIMEOUT = 10s;

// ============== 全局计数器 ==============
std::atomic<int64_t> g_sent{0};
std::atomic<int64_t> g_received{0};
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
    g_consumer_done = false;
    g_consumer_ready = false;
    g_consumer_done_latch = nullptr;
    g_received_set.clear();
}

struct ThroughputSample {
    double elapsed_ms;
    double throughput;
    int64_t received;
};

// ============== 消费者协程 ==============

// 简单消费者（吞吐量测试）
Task<void> simpleConsumer(galay::mpsc::UnboundedChannel<int64_t>* channel, int64_t expected_count) {
    g_consumer_ready.store(true, std::memory_order_release);
    int64_t received = 0;
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
    }
    g_received.store(received, std::memory_order_relaxed);
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

// ============== 压测函数 ==============

// 1. 多生产者吞吐量测试
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
        ParallelScheduler scheduler;

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

// 2. 正确性验证测试
void benchCorrectness(int producer_count, int64_t total_messages) {
    LogInfo("--- Correctness Test ({} producers, {} messages) ---",
            producer_count, total_messages);
    resetCounters();

    galay::mpsc::UnboundedChannel<int64_t> channel;
    ParallelScheduler scheduler;

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

// 3. 持续压力测试
void benchSustained(int duration_sec) {
    LogInfo("--- Sustained Load Test ({}s) ---", duration_sec);
    resetCounters();

    galay::mpsc::UnboundedChannel<int64_t> channel;
    ParallelScheduler scheduler;

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

    // 1. 多生产者吞吐量
    benchMultiProducerThroughput(4, THROUGHPUT_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    // 2. 正确性验证
    benchCorrectness(4, CORRECTNESS_MESSAGES);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    // 3. 持续压力测试
    benchSustained(5);
    if (g_benchmark_failed.load(std::memory_order_acquire)) return 1;
    LogInfo("");

    LogInfo("=== Benchmark Complete ===");

    return 0;
}
