/**
 * @file b23_bounded_channel_throughput.cc
 * @brief 有界 MPMC channel 吞吐、延迟和关闭收尾压测。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "benchmark/cpp/common/benchmark_affinity.h"
#include "benchmark/cpp/common/benchmark_sync.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

constexpr int64_t kMessages = 1'000'000;
constexpr int kWarmupSamples = 1;
constexpr int kSamples = 5;

struct TimestampedMessage {
    int64_t id = 0;
    std::chrono::steady_clock::time_point sent_at{};
};

struct ThroughputResult {
    double messagesPerSecond = 0.0;
    int64_t received = 0;
    galay::benchmark::ThreadPlacement placement =
        galay::benchmark::ThreadPlacement::kUnsupported;
};

struct LatencyStats {
    galay::benchmark::ThreadPlacement placement =
        galay::benchmark::ThreadPlacement::kUnsupported;
    size_t samples = 0;
    double averageUs = 0.0;
    double p50Us = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
    double p999Us = 0.0;
};

LatencyStats summarizeLatencies(std::vector<int64_t> latencies)
{
    if (latencies.empty()) {
        return {};
    }

    std::sort(latencies.begin(), latencies.end());
    long double totalNs = 0.0;
    for (const int64_t latency : latencies) {
        totalNs += static_cast<long double>(latency);
    }

    const auto percentileUs = [&latencies](double percentile) {
        const size_t index = static_cast<size_t>(
            percentile * static_cast<double>(latencies.size() - 1));
        return static_cast<double>(latencies[index]) / 1000.0;
    };

    return {
        .samples = latencies.size(),
        .averageUs = static_cast<double>(totalNs / latencies.size()) / 1000.0,
        .p50Us = percentileUs(0.50),
        .p95Us = percentileUs(0.95),
        .p99Us = percentileUs(0.99),
        .p999Us = percentileUs(0.999),
    };
}

template <typename Receive, typename Send>
ThroughputResult runThreaded(int producerCount,
                             int consumerCount,
                             int64_t totalMessages,
                             Receive receive,
                             Send send)
{
    std::atomic<bool> producersDone{false};
    galay::benchmark::CompletionLatch consumerDone(static_cast<size_t>(consumerCount));
    galay::benchmark::CompletionLatch allThreadsReady(
        static_cast<size_t>(producerCount + consumerCount));
    galay::benchmark::StartGate startGate;
    std::vector<int64_t> receivedByConsumer(static_cast<size_t>(consumerCount), 0);
    std::vector<galay::benchmark::ThreadPlacement> placement(
        static_cast<size_t>(producerCount + consumerCount),
        galay::benchmark::ThreadPlacement::kUnsupported);
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    const int64_t perProducer = totalMessages / producerCount;

    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            placement[static_cast<size_t>(producer)] =
                galay::benchmark::pinCurrentThread(static_cast<size_t>(producer));
            allThreadsReady.arrive();
            startGate.wait();
            const int64_t first = producer * perProducer;
            for (int64_t i = 0; i < perProducer; ++i) {
                while (!send(first + i)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (int consumer = 0; consumer < consumerCount; ++consumer) {
        consumers.emplace_back([&, consumer]() {
            placement[static_cast<size_t>(producerCount + consumer)] =
                galay::benchmark::pinCurrentThread(
                    static_cast<size_t>(producerCount + consumer));
            allThreadsReady.arrive();
            startGate.wait();
            int64_t localReceived = 0;
            for (;;) {
                auto value = receive();
                if (value.has_value()) {
                    ++localReceived;
                    continue;
                }
                if (producersDone.load(std::memory_order_acquire)) {
                    receivedByConsumer[static_cast<size_t>(consumer)] = localReceived;
                    consumerDone.arrive();
                    return;
                }
                std::this_thread::yield();
            }
        });
    }
    allThreadsReady.wait();
    const auto start = std::chrono::steady_clock::now();
    startGate.open();
    for (auto& producer : producers) {
        producer.join();
    }
    producersDone.store(true, std::memory_order_release);
    consumerDone.wait();
    for (auto& consumer : consumers) {
        consumer.join();
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    int64_t receivedCount = 0;
    for (const int64_t count : receivedByConsumer) {
        receivedCount += count;
    }
    // 取最弱的放置结果：只要有线程没绑上，整轮数据就不能当作已绑核。
    auto weakest = galay::benchmark::ThreadPlacement::kPinnedToCore;
    for (const auto value : placement) {
        if (value == galay::benchmark::ThreadPlacement::kUnsupported) {
            weakest = value;
            break;
        }
        if (value == galay::benchmark::ThreadPlacement::kPerformanceClassOnly) {
            weakest = value;
        }
    }
    return {
        .messagesPerSecond = elapsedNs > 0
            ? static_cast<double>(totalMessages) * 1'000'000'000.0 / elapsedNs
            : 0.0,
        .received = receivedCount,
        .placement = weakest,
    };
}

template <typename Channel>
ThroughputResult runBounded(int producerCount, int consumerCount, size_t capacity)
{
    Channel channel(capacity);
    return runThreaded(
        producerCount,
        consumerCount,
        kMessages,
        [&channel]() { return channel.tryRecv(); },
        [&channel](int64_t value) { return channel.trySend(std::move(value)); });
}

ThroughputResult runMpsc(int producerCount)
{
    galay::mpsc::UnboundedChannel<int64_t> channel;
    return runThreaded(
        producerCount,
        1,
        kMessages,
        [&channel]() { return channel.tryRecv(); },
        [&channel](int64_t value) { return channel.send(std::move(value)); });
}

LatencyStats runLatency(size_t capacity, int64_t messageCount)
{
    galay::mpmc::BoundedChannel<TimestampedMessage> channel(capacity);
    std::atomic<bool> producerDone{false};
    std::vector<int64_t> latencies;
    latencies.reserve(static_cast<size_t>(messageCount));

    std::atomic<galay::benchmark::ThreadPlacement> consumerPlacement{
        galay::benchmark::ThreadPlacement::kUnsupported};
    std::thread consumer([&]() {
        consumerPlacement.store(galay::benchmark::pinCurrentThread(1),
                                std::memory_order_release);
        int64_t received = 0;
        while (!producerDone.load(std::memory_order_acquire) || received < messageCount) {
            auto message = channel.tryRecv();
            if (!message.has_value()) {
                std::this_thread::yield();
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - message->sent_at).count());
            ++received;
        }
    });

    const auto latencyPlacement = galay::benchmark::pinCurrentThread(0);
    for (int64_t i = 0; i < messageCount; ++i) {
        TimestampedMessage message{.id = i, .sent_at = std::chrono::steady_clock::now()};
        while (!channel.trySend(std::move(message))) {
            std::this_thread::yield();
        }
    }
    producerDone.store(true, std::memory_order_release);
    consumer.join();
    auto stats = summarizeLatencies(std::move(latencies));
    // 两个线程取较弱的放置结果，避免只有一端绑上就标成已绑核。
    const auto consumerResult = consumerPlacement.load(std::memory_order_acquire);
    stats.placement =
        consumerResult == galay::benchmark::ThreadPlacement::kPinnedToCore
            ? latencyPlacement
            : consumerResult;
    return stats;
}

struct AsyncBenchmarkState {
    std::atomic<bool> ready{false};
    galay::benchmark::CompletionLatch done{1};
    std::atomic<int64_t> received{0};
};

Task<void> asyncConsumer(galay::mpmc::BoundedChannel<int64_t>* channel,
                         int64_t messageCount,
                         AsyncBenchmarkState* state)
{
    state->ready.store(true, std::memory_order_release);
    for (int64_t i = 0; i < messageCount; ++i) {
        auto value = co_await channel->recv();
        if (value) {
            state->received.fetch_add(1, std::memory_order_relaxed);
        }
    }
    state->done.arrive();
    co_return;
}

double runAsyncHandoff(size_t capacity, int64_t messageCount)
{
    galay::mpmc::BoundedChannel<int64_t> channel(capacity);
    AsyncBenchmarkState state;
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started || !scheduleTask(scheduler, asyncConsumer(&channel, messageCount, &state)) ||
        !galay::benchmark::waitForFlag(state.ready, 2s)) {
        scheduler.stop();
        return 0.0;
    }
    const auto start = std::chrono::steady_clock::now();
    std::thread producer([&]() {
        for (int64_t i = 0; i < messageCount; ++i) {
            while (!channel.trySend(i)) {
                std::this_thread::yield();
            }
        }
    });
    state.done.wait();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    producer.join();
    scheduler.stop();
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return elapsedNs > 0
        ? static_cast<double>(messageCount) * 1'000'000'000.0 / elapsedNs
        : 0.0;
}

bool runCloseCheck()
{
    galay::mpmc::BoundedChannel<int64_t> channel(256);
    std::atomic<int64_t> received{0};
    std::thread consumer([&]() {
        for (;;) {
            auto value = channel.tryRecv();
            if (value.has_value()) {
                received.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (channel.isClosed()) {
                return;
            }
            std::this_thread::yield();
        }
    });

    for (int64_t i = 0; i < kMessages / 10; ++i) {
        while (!channel.trySend(i)) {
            std::this_thread::yield();
        }
    }
    channel.close();
    channel.close();
    consumer.join();
    return received.load(std::memory_order_acquire) == kMessages / 10;
}

void printThroughput(const char* name, int producerCount, int consumerCount, size_t capacity)
{
    for (int sample = 0; sample < kWarmupSamples; ++sample) {
        const auto warmup = runBounded<galay::mpmc::BoundedChannel<int64_t>>(
            producerCount, consumerCount, capacity);
        if (warmup.received != kMessages) {
            std::cout << name << " warmup_failed received=" << warmup.received << '\n';
            return;
        }
    }

    std::vector<double> samples;
    samples.reserve(kSamples);
    int64_t received = 0;
    auto placement = galay::benchmark::ThreadPlacement::kUnsupported;
    for (int sample = 0; sample < kSamples; ++sample) {
        const auto result = runBounded<galay::mpmc::BoundedChannel<int64_t>>(
            producerCount, consumerCount, capacity);
        samples.push_back(result.messagesPerSecond);
        received = result.received;
        placement = result.placement;
    }
    std::cout << name << " producers=" << producerCount
              << " consumers=" << consumerCount
              << " capacity=" << capacity
              << " median_msg_s="
              << galay::benchmark::medianElement(std::move(samples))
              << " received=" << received
              << " placement=" << galay::benchmark::threadPlacementName(placement)
              << '\n';
}

void printMpscThroughput(int producerCount)
{
    for (int sample = 0; sample < kWarmupSamples; ++sample) {
        const auto warmup = runMpsc(producerCount);
        if (warmup.received != kMessages) {
            std::cout << "mpsc warmup_failed received=" << warmup.received << '\n';
            return;
        }
    }

    std::vector<double> samples;
    samples.reserve(kSamples);
    int64_t received = 0;
    for (int sample = 0; sample < kSamples; ++sample) {
        const auto result = runMpsc(producerCount);
        samples.push_back(result.messagesPerSecond);
        received = result.received;
    }
    std::cout << "mpsc producers=" << producerCount
              << " median_msg_s="
              << galay::benchmark::medianElement(std::move(samples))
              << " received=" << received << '\n';
}

} // namespace

int main()
{
    printThroughput("bounded", 1, 1, 256);
    printThroughput("bounded", 1, 1, 4096);
    printThroughput("bounded", 4, 1, 256);
    printThroughput("bounded", 1, 4, 256);
    printThroughput("bounded", 4, 4, 256);
    printThroughput("bounded", 4, 4, 4096);
    printMpscThroughput(2);
    printMpscThroughput(4);
    const double asyncWarmup = runAsyncHandoff(256, kMessages);
    if (asyncWarmup <= 0.0) {
        std::cout << "bounded async_handoff warmup_failed\n";
        return 1;
    }
    std::cout << "bounded async_handoff capacity=256 median_msg_s="
              << galay::benchmark::medianElement(std::vector<double>{
                     runAsyncHandoff(256, kMessages),
                     runAsyncHandoff(256, kMessages),
                     runAsyncHandoff(256, kMessages)}) << '\n';
    const auto latencyWarmup = runLatency(256, 10'000);
    if (latencyWarmup.samples != 10'000) {
        std::cout << "bounded latency warmup_failed samples=" << latencyWarmup.samples << '\n';
        return 1;
    }
    const auto latency = runLatency(256, 50'000);
    std::cout << "bounded capacity=256 latency_samples=" << latency.samples
              << " avg_latency_us=" << latency.averageUs
              << " p50_latency_us=" << latency.p50Us
              << " p95_latency_us=" << latency.p95Us
              << " p99_latency_us=" << latency.p99Us
              << " p999_latency_us=" << latency.p999Us
              << " placement="
              << galay::benchmark::threadPlacementName(latency.placement) << '\n';
    std::cout << "close_check=" << (runCloseCheck() ? "PASS" : "FAIL") << '\n';
    return 0;
}
