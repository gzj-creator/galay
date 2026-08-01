/**
 * @file b24_channel_family_throughput.cc
 * @brief 压测 bounded MPMC 在多生产者、多消费者拓扑下的同步吞吐。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h>
#include "benchmark/cpp/common/benchmark_affinity.h"
#include "benchmark/cpp/common/benchmark_sync.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
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
    uint64_t fullRetries = 0;
    uint64_t emptyRetries = 0;
    galay::benchmark::ThreadPlacement placement =
        galay::benchmark::ThreadPlacement::kUnsupported;
};

Measurement runChannel(int producerCount, int consumerCount, size_t capacity)
{
    galay::mpmc::BoundedChannel<int64_t> channel(capacity);
    galay::benchmark::CompletionLatch ready(
        static_cast<size_t>(producerCount + consumerCount));
    galay::benchmark::StartGate start;
    std::vector<uint64_t> fullRetries(static_cast<size_t>(producerCount), 0);
    std::vector<uint64_t> emptyRetries(static_cast<size_t>(consumerCount), 0);
    std::vector<int64_t> receivedByConsumer(static_cast<size_t>(consumerCount), 0);
    std::vector<int64_t> sumByConsumer(static_cast<size_t>(consumerCount), 0);
    std::vector<galay::benchmark::ThreadPlacement> placements(
        static_cast<size_t>(producerCount + consumerCount),
        galay::benchmark::ThreadPlacement::kUnsupported);
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(static_cast<size_t>(producerCount));
    consumers.reserve(static_cast<size_t>(consumerCount));

    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            placements[static_cast<size_t>(producer)] =
                galay::benchmark::pinCurrentThread(static_cast<size_t>(producer));
            ready.arrive();
            start.wait();
            const int64_t first = kMessages * producer / producerCount;
            const int64_t last = kMessages * (producer + 1) / producerCount;
            uint64_t localFullRetries = 0;
            for (int64_t valueId = first; valueId < last; ++valueId) {
                int64_t value = valueId;
                while (!channel.trySend(std::move(value))) {
                    ++localFullRetries;
                    std::this_thread::yield();
                }
            }
            fullRetries[static_cast<size_t>(producer)] = localFullRetries;
        });
    }

    for (int consumer = 0; consumer < consumerCount; ++consumer) {
        consumers.emplace_back([&, consumer]() {
            placements[static_cast<size_t>(producerCount + consumer)] =
                galay::benchmark::pinCurrentThread(
                    static_cast<size_t>(producerCount + consumer));
            ready.arrive();
            start.wait();
            int64_t localReceived = 0;
            int64_t localSum = 0;
            uint64_t localEmptyRetries = 0;
            for (;;) {
                auto value = channel.tryRecv();
                if (value.has_value()) {
                    ++localReceived;
                    localSum += *value;
                    continue;
                }
                if (channel.isClosed()) {
                    break;
                }
                ++localEmptyRetries;
                std::this_thread::yield();
            }
            receivedByConsumer[static_cast<size_t>(consumer)] = localReceived;
            sumByConsumer[static_cast<size_t>(consumer)] = localSum;
            emptyRetries[static_cast<size_t>(consumer)] = localEmptyRetries;
        });
    }

    ready.wait();
    const auto begin = std::chrono::steady_clock::now();
    start.open();
    for (std::thread& producer : producers) {
        producer.join();
    }
    channel.close();
    for (std::thread& consumer : consumers) {
        consumer.join();
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto elapsedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    auto placement = galay::benchmark::ThreadPlacement::kPinnedToCore;
    for (const auto threadPlacement : placements) {
        placement = std::max(placement, threadPlacement);
    }

    return {
        .messagesPerSecond = elapsedNs > 0
            ? static_cast<double>(kMessages) * 1'000'000'000.0 / elapsedNs
            : 0.0,
        .received = std::accumulate(
            receivedByConsumer.begin(), receivedByConsumer.end(), int64_t{0}),
        .sum = std::accumulate(sumByConsumer.begin(), sumByConsumer.end(), int64_t{0}),
        .fullRetries =
            std::accumulate(fullRetries.begin(), fullRetries.end(), uint64_t{0}),
        .emptyRetries =
            std::accumulate(emptyRetries.begin(), emptyRetries.end(), uint64_t{0}),
        .placement = placement,
    };
}

bool validMeasurement(const Measurement& measurement)
{
    bool placementValid = true;
#if defined(__APPLE__)
    placementValid = measurement.placement ==
        galay::benchmark::ThreadPlacement::kPerformanceClassOnly;
#elif defined(__linux__)
    placementValid =
        measurement.placement == galay::benchmark::ThreadPlacement::kPinnedToCore;
#endif
    return placementValid && measurement.messagesPerSecond > 0.0 &&
        measurement.received == kMessages && measurement.sum == kExpectedSum;
}

void printSummary(const char* topology,
                  int producerCount,
                  int consumerCount,
                  size_t capacity,
                  const std::vector<Measurement>& samples)
{
    std::vector<double> throughput;
    std::vector<uint64_t> fullRetries;
    std::vector<uint64_t> emptyRetries;
    auto placement = galay::benchmark::ThreadPlacement::kPinnedToCore;
    throughput.reserve(samples.size());
    fullRetries.reserve(samples.size());
    emptyRetries.reserve(samples.size());
    for (const Measurement& sample : samples) {
        throughput.push_back(sample.messagesPerSecond);
        fullRetries.push_back(sample.fullRetries);
        emptyRetries.push_back(sample.emptyRetries);
        placement = std::max(placement, sample.placement);
    }

    const auto [minimum, maximum] = std::minmax_element(
        throughput.begin(), throughput.end());
    const double minimumThroughput = *minimum;
    const double maximumThroughput = *maximum;
    std::cout << "mpmc_bounded topology=" << topology
              << " producers=" << producerCount
              << " consumers=" << consumerCount
              << " capacity=" << capacity
              << " messages=" << kMessages
              << " samples=" << samples.size()
              << " median_msg_s="
              << galay::benchmark::medianElement(std::move(throughput))
              << " min_msg_s=" << minimumThroughput
              << " max_msg_s=" << maximumThroughput
              << " median_full_retries="
              << galay::benchmark::medianElement(std::move(fullRetries))
              << " median_empty_retries="
              << galay::benchmark::medianElement(std::move(emptyRetries))
              << " placement=" << galay::benchmark::threadPlacementName(placement)
              << '\n';
}

bool runCase(const char* topology,
             int producerCount,
             int consumerCount,
             size_t capacity)
{
    for (int warmup = 0; warmup < kWarmupSamples; ++warmup) {
        if (!validMeasurement(runChannel(producerCount, consumerCount, capacity))) {
            std::cout << "mpmc_bounded warmup_failed topology=" << topology
                      << " capacity=" << capacity << '\n';
            return false;
        }
    }

    std::vector<Measurement> samples;
    samples.reserve(kSamples);
    for (int sample = 0; sample < kSamples; ++sample) {
        Measurement measurement = runChannel(producerCount, consumerCount, capacity);
        if (!validMeasurement(measurement)) {
            std::cout << "mpmc_bounded sample_failed topology=" << topology
                      << " capacity=" << capacity
                      << " sample=" << sample << '\n';
            return false;
        }
        samples.push_back(std::move(measurement));
    }
    printSummary(topology, producerCount, consumerCount, capacity, samples);
    return true;
}

} // namespace

int main()
{
    bool ok = true;
    for (const size_t capacity : {size_t{256}, size_t{4096}}) {
        ok = runCase("2p2c", 2, 2, capacity) && ok;
        ok = runCase("4p4c", 4, 4, capacity) && ok;
    }
    return ok ? 0 : 1;
}
