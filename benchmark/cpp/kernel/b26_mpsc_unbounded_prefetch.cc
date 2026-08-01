/**
 * @file b26_mpsc_unbounded_prefetch.cc
 * @brief 隔离测量 MPSC unbounded 单消费者预取上限的吞吐影响。
 */

#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include "benchmark/cpp/common/benchmark_sync.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int64_t kMessages = 5'000'000;
constexpr int kWarmupSamples = 1;
constexpr int kSamples = 7;
constexpr int64_t kExpectedSum = (kMessages - 1) * kMessages / 2;

struct Measurement
{
    double messagesPerSecond = 0.0;
    int64_t received = 0;
    int64_t sum = 0;
    uint64_t emptyRetries = 0;
    bool sendOk = false;
};

Measurement runSample(size_t prefetchLimit, int producerCount)
{
    using Channel = galay::mpsc::UnboundedChannel<int64_t>;
    Channel channel(Channel::DEFAULT_BATCH_SIZE, prefetchLimit);
    std::vector<Channel::ProducerToken> tokens;
    tokens.reserve(static_cast<size_t>(producerCount));
    for (int producer = 0; producer < producerCount; ++producer) {
        auto token = channel.makeProducerToken();
        if (!token.valid()) {
            return {};
        }
        tokens.push_back(std::move(token));
    }

    galay::benchmark::CompletionLatch ready(static_cast<size_t>(producerCount + 1));
    galay::benchmark::StartGate start;
    std::vector<std::thread> producers;
    producers.reserve(static_cast<size_t>(producerCount));
    int64_t received = 0;
    int64_t sum = 0;
    uint64_t emptyRetries = 0;
    std::atomic<bool> sendFailed{false};

    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            ready.arrive();
            start.wait();
            const int64_t first = kMessages * producer / producerCount;
            const int64_t last = kMessages * (producer + 1) / producerCount;
            for (int64_t valueId = first; valueId < last; ++valueId) {
                int64_t value = valueId;
                if (!channel.send(tokens[static_cast<size_t>(producer)],
                                  std::move(value))) {
                    sendFailed.store(true, std::memory_order_release);
                    break;
                }
            }
        });
    }

    std::thread consumer([&]() {
        ready.arrive();
        start.wait();
        int64_t localReceived = 0;
        int64_t localSum = 0;
        uint64_t localEmptyRetries = 0;
        while (localReceived < kMessages &&
               !sendFailed.load(std::memory_order_acquire)) {
            auto value = channel.tryRecv();
            if (value.has_value()) {
                ++localReceived;
                localSum += *value;
            } else {
                ++localEmptyRetries;
                std::this_thread::yield();
            }
        }
        received = localReceived;
        sum = localSum;
        emptyRetries = localEmptyRetries;
    });

    ready.wait();
    const auto begin = std::chrono::steady_clock::now();
    start.open();
    for (std::thread& producer : producers) {
        producer.join();
    }
    consumer.join();
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();

    return {
        .messagesPerSecond = elapsedNs > 0
            ? static_cast<double>(kMessages) * 1'000'000'000.0 / elapsedNs
            : 0.0,
        .received = received,
        .sum = sum,
        .emptyRetries = emptyRetries,
        .sendOk = !sendFailed.load(std::memory_order_acquire),
    };
}

bool valid(const Measurement& measurement)
{
    return measurement.messagesPerSecond > 0.0 &&
        measurement.received == kMessages && measurement.sum == kExpectedSum &&
        measurement.sendOk;
}

bool runCase(size_t prefetchLimit, int producerCount)
{
    for (int warmup = 0; warmup < kWarmupSamples; ++warmup) {
        if (!valid(runSample(prefetchLimit, producerCount))) {
            std::cerr << "mpsc prefetch warmup failed: producers=" << producerCount
                      << " prefetch=" << prefetchLimit << '\n';
            return false;
        }
    }

    std::vector<double> throughput;
    std::vector<uint64_t> emptyRetries;
    throughput.reserve(kSamples);
    emptyRetries.reserve(kSamples);
    for (int sample = 0; sample < kSamples; ++sample) {
        const Measurement measurement = runSample(prefetchLimit, producerCount);
        if (!valid(measurement)) {
            std::cerr << "mpsc prefetch sample failed: producers=" << producerCount
                      << " prefetch=" << prefetchLimit
                      << " sample=" << sample << '\n';
            return false;
        }
        throughput.push_back(measurement.messagesPerSecond);
        emptyRetries.push_back(measurement.emptyRetries);
    }

    std::cout << "mpsc_token topology="
              << (producerCount == 1 ? "1p1c" : "4p1c")
              << " producers=" << producerCount
              << " prefetch=" << prefetchLimit
              << " messages=" << kMessages
              << " samples=" << kSamples
              << " median_msg_s="
              << galay::benchmark::medianElement(std::move(throughput))
              << " median_empty_retries="
              << galay::benchmark::medianElement(std::move(emptyRetries))
              << '\n';
    return true;
}

}  // namespace

int main()
{
    constexpr std::array<size_t, 4> kPrefetchLimits{0, 1, 4, 16};
    constexpr std::array<int, 2> kProducerCounts{1, 4};
    for (const int producerCount : kProducerCounts) {
        for (const size_t prefetchLimit : kPrefetchLimits) {
            if (!runCase(prefetchLimit, producerCount)) {
                return 1;
            }
        }
    }
    return 0;
}
