/**
 * @file b25_unbounded_channel_family_throughput.cc
 * @brief 压测 unbounded MPMC wrapper 与 raw moodycamel 数据面的同步吞吐。
 */

#include <galay/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h>
#include "benchmark/cpp/common/benchmark_affinity.h"
#include "benchmark/cpp/common/benchmark_sync.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace galay::mpmc {

/** @brief 仅供 B25 隔离同一 DataQueue 的 raw 与 send-permit 成本。 */
struct UnboundedChannelTestAccess
{
    template <UnboundedValue T>
    static bool rawSend(UnboundedChannel<T>& channel,
                        typename UnboundedChannel<T>::ProducerToken& token,
                        T&& value)
    {
        return token.validFor(channel) && channel.m_queue.enqueue(
            token.m_epochNode->queueToken, std::move(value));
    }

    template <UnboundedValue T>
    static bool rawRecv(UnboundedChannel<T>& channel,
                        typename UnboundedChannel<T>::ConsumerToken& token,
                        T& value)
    {
        return token.validFor(channel) &&
            channel.m_queue.try_dequeue(token.m_token, value);
    }
};

} // namespace galay::mpmc

namespace {

constexpr int64_t kMessages = 5'000'000;
constexpr int kWarmupSamples = 1;
constexpr int kSamples = 7;
constexpr int64_t kExpectedSum = (kMessages - 1) * kMessages / 2;
using DefaultQueueTraits = moodycamel::ConcurrentQueueDefaultTraits;
constexpr size_t kInitialPoolElements =
    galay::mpmc::detail::kUnboundedInitialPoolElements;

enum class ChannelPath : uint8_t {
    kRaw,
    kToken,
};

struct Measurement
{
    double messagesPerSecond = 0.0;
    int64_t received = 0;
    int64_t sum = 0;
    uint64_t sendRetries = 0;
    uint64_t emptyRetries = 0;
    size_t finalSize = 0;
    galay::benchmark::ThreadPlacement placement =
        galay::benchmark::ThreadPlacement::kUnsupported;
    bool setupFailed = false;
};

template <ChannelPath Path>
Measurement runChannel(int producerCount, int consumerCount)
{
    using Channel = galay::mpmc::UnboundedChannel<int64_t>;
    Channel channel;
    std::atomic<bool> setupFailed{false};
    galay::benchmark::CompletionLatch ready(
        static_cast<size_t>(producerCount + consumerCount));
    galay::benchmark::StartGate start;
    std::vector<uint64_t> sendRetries(static_cast<size_t>(producerCount), 0);
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
            auto token = channel.makeProducerToken();
            if (!token.valid()) {
                setupFailed.store(true, std::memory_order_release);
            }
            ready.arrive();
            start.wait();
            if (setupFailed.load(std::memory_order_acquire)) {
                return;
            }
            const int64_t first = kMessages * producer / producerCount;
            const int64_t last = kMessages * (producer + 1) / producerCount;
            uint64_t localRetries = 0;
            for (int64_t valueId = first; valueId < last; ++valueId) {
                int64_t value = valueId;
                bool sent = false;
                do {
                    if constexpr (Path == ChannelPath::kRaw) {
                        sent = galay::mpmc::UnboundedChannelTestAccess::rawSend(
                            channel, token, std::move(value));
                    } else {
                        sent = channel.send(token, std::move(value));
                    }
                    if (!sent) {
                        ++localRetries;
                        std::this_thread::yield();
                    }
                } while (!sent);
            }
            sendRetries[static_cast<size_t>(producer)] = localRetries;
        });
    }

    for (int consumer = 0; consumer < consumerCount; ++consumer) {
        consumers.emplace_back([&, consumer]() {
            placements[static_cast<size_t>(producerCount + consumer)] =
                galay::benchmark::pinCurrentThread(
                    static_cast<size_t>(producerCount + consumer));
            auto token = channel.makeConsumerToken();
            if (!token.valid()) {
                setupFailed.store(true, std::memory_order_release);
            }
            ready.arrive();
            start.wait();
            if (setupFailed.load(std::memory_order_acquire)) {
                return;
            }
            int64_t localReceived = 0;
            int64_t localSum = 0;
            uint64_t localEmptyRetries = 0;
            for (;;) {
                int64_t value = 0;
                bool received = false;
                if constexpr (Path == ChannelPath::kRaw) {
                    received = galay::mpmc::UnboundedChannelTestAccess::rawRecv(
                        channel, token, value);
                } else {
                    std::optional<int64_t> result = channel.tryRecv(token);
                    if (result.has_value()) {
                        value = *result;
                        received = true;
                    }
                }
                if (received) {
                    ++localReceived;
                    localSum += value;
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
        .sendRetries =
            std::accumulate(sendRetries.begin(), sendRetries.end(), uint64_t{0}),
        .emptyRetries =
            std::accumulate(emptyRetries.begin(), emptyRetries.end(), uint64_t{0}),
        .finalSize = channel.size(),
        .placement = placement,
        .setupFailed = setupFailed.load(std::memory_order_acquire),
    };
}

/** @brief 在相同 MPMC harness 中测量指定 traits 的独立 raw moodycamel 队列。 */
template <typename QueueTraits>
Measurement runRawQueue(int producerCount, int consumerCount)
{
    using Queue = moodycamel::ConcurrentQueue<int64_t, QueueTraits>;
    static_assert(kInitialPoolElements % QueueTraits::BLOCK_SIZE == 0);
    Queue queue(kInitialPoolElements);
    std::atomic<bool> closed{false};
    std::atomic<bool> setupFailed{false};
    galay::benchmark::CompletionLatch ready(
        static_cast<size_t>(producerCount + consumerCount));
    galay::benchmark::StartGate start;
    std::vector<uint64_t> sendRetries(static_cast<size_t>(producerCount), 0);
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
            moodycamel::ProducerToken token(queue);
            if (!token.valid()) {
                setupFailed.store(true, std::memory_order_release);
            }
            ready.arrive();
            start.wait();
            if (setupFailed.load(std::memory_order_acquire)) {
                return;
            }
            const int64_t first = kMessages * producer / producerCount;
            const int64_t last = kMessages * (producer + 1) / producerCount;
            uint64_t localRetries = 0;
            for (int64_t valueId = first; valueId < last; ++valueId) {
                int64_t value = valueId;
                bool sent = queue.enqueue(token, std::move(value));
                while (!sent) {
                    ++localRetries;
                    std::this_thread::yield();
                    sent = queue.enqueue(token, std::move(value));
                }
            }
            sendRetries[static_cast<size_t>(producer)] = localRetries;
        });
    }

    for (int consumer = 0; consumer < consumerCount; ++consumer) {
        consumers.emplace_back([&, consumer]() {
            placements[static_cast<size_t>(producerCount + consumer)] =
                galay::benchmark::pinCurrentThread(
                    static_cast<size_t>(producerCount + consumer));
            moodycamel::ConsumerToken token(queue);
            ready.arrive();
            start.wait();
            if (setupFailed.load(std::memory_order_acquire)) {
                return;
            }
            int64_t localReceived = 0;
            int64_t localSum = 0;
            uint64_t localEmptyRetries = 0;
            int64_t value = 0;
            for (;;) {
                if (queue.try_dequeue(token, value)) {
                    ++localReceived;
                    localSum += value;
                    continue;
                }
                if (closed.load(std::memory_order_acquire)) {
                    if (queue.try_dequeue(token, value)) {
                        ++localReceived;
                        localSum += value;
                        continue;
                    }
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
    closed.store(true, std::memory_order_release);
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
        .sendRetries =
            std::accumulate(sendRetries.begin(), sendRetries.end(), uint64_t{0}),
        .emptyRetries =
            std::accumulate(emptyRetries.begin(), emptyRetries.end(), uint64_t{0}),
        .finalSize = queue.size_approx(),
        .placement = placement,
        .setupFailed = setupFailed.load(std::memory_order_acquire),
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
    return !measurement.setupFailed && placementValid &&
        measurement.messagesPerSecond > 0.0 &&
        measurement.received == kMessages && measurement.sum == kExpectedSum &&
        measurement.sendRetries == 0 && measurement.finalSize == 0;
}

void printSummary(const char* name,
                  const char* topology,
                  int producerCount,
                  int consumerCount,
                  size_t blockSize,
                  size_t emptyCounterThreshold,
                  size_t initialIndexSize,
                  size_t implicitProducerHashSize,
                  uint32_t consumerRotationQuota,
                  const std::vector<Measurement>& samples)
{
    std::vector<double> throughput;
    std::vector<uint64_t> sendRetries;
    std::vector<uint64_t> emptyRetries;
    auto placement = galay::benchmark::ThreadPlacement::kPinnedToCore;
    throughput.reserve(samples.size());
    sendRetries.reserve(samples.size());
    emptyRetries.reserve(samples.size());
    for (const Measurement& sample : samples) {
        throughput.push_back(sample.messagesPerSecond);
        sendRetries.push_back(sample.sendRetries);
        emptyRetries.push_back(sample.emptyRetries);
        placement = std::max(placement, sample.placement);
    }

    const auto [minimum, maximum] = std::minmax_element(
        throughput.begin(), throughput.end());
    const double minimumThroughput = *minimum;
    const double maximumThroughput = *maximum;
    std::cout << name << " topology=" << topology
              << " producers=" << producerCount
              << " consumers=" << consumerCount
              << " capacity=unbounded"
              << " block_size=" << blockSize
              << " empty_counter_threshold=" << emptyCounterThreshold
              << " explicit_initial_index_size=" << initialIndexSize
              << " implicit_producer_hash_size=" << implicitProducerHashSize
              << " initial_pool_elements=" << kInitialPoolElements
              << " initial_pool_blocks="
              << (kInitialPoolElements / blockSize)
              << " consumer_rotation_quota=" << consumerRotationQuota
              << " measurement_scope=data_plane"
              << " messages=" << kMessages
              << " samples=" << samples.size()
              << " median_msg_s="
              << galay::benchmark::medianElement(std::move(throughput))
              << " min_msg_s=" << minimumThroughput
              << " max_msg_s=" << maximumThroughput
              << " median_send_retries="
              << galay::benchmark::medianElement(std::move(sendRetries))
              << " median_empty_retries="
              << galay::benchmark::medianElement(std::move(emptyRetries))
              << " placement=" << galay::benchmark::threadPlacementName(placement)
              << '\n';
}

bool printRatioSummary(const char* name,
                       const char* topology,
                       const std::vector<Measurement>& numerator,
                       const std::vector<Measurement>& denominator)
{
    if (numerator.size() != kSamples || denominator.size() != kSamples) {
        std::cout << name << " invalid_sample_count topology=" << topology << '\n';
        return false;
    }
    std::vector<double> ratios;
    ratios.reserve(numerator.size());
    for (size_t index = 0; index < numerator.size(); ++index) {
        if (denominator[index].messagesPerSecond <= 0.0) {
            std::cout << name << " invalid_denominator topology=" << topology
                      << " sample=" << index << '\n';
            return false;
        }
        ratios.push_back(
            numerator[index].messagesPerSecond / denominator[index].messagesPerSecond);
    }
    const auto [minimum, maximum] = std::minmax_element(ratios.begin(), ratios.end());
    const double minimumRatio = *minimum;
    const double maximumRatio = *maximum;
    std::cout << name << " topology=" << topology
              << " samples=" << ratios.size()
              << " median_ratio="
              << galay::benchmark::medianElement(std::move(ratios))
              << " min_ratio=" << minimumRatio
              << " max_ratio=" << maximumRatio << '\n';
    return true;
}

using Runner = Measurement (*)(int, int);

struct TraitCase
{
    const char* name;
    size_t blockSize;
    size_t emptyCounterThreshold;
    size_t initialIndexSize;
    size_t implicitProducerHashSize;
    uint32_t consumerRotationQuota;
    Runner runner;
};

template <size_t CaseCount>
bool runCases(const char* topology,
              int producerCount,
              int consumerCount,
              const std::array<TraitCase, CaseCount>& cases,
              std::array<std::vector<Measurement>, CaseCount>& samples)
{
    size_t round = 0;
    for (int warmup = 0; warmup < kWarmupSamples; ++warmup) {
        for (size_t offset = 0; offset < CaseCount; ++offset) {
            const size_t index = (round + offset) % CaseCount;
            if (!validMeasurement(
                    cases[index].runner(producerCount, consumerCount))) {
                std::cout << cases[index].name
                          << " warmup_failed topology=" << topology
                          << '\n';
                return false;
            }
        }
        ++round;
    }

    for (auto& caseSamples : samples) {
        caseSamples.reserve(kSamples);
    }
    for (int sample = 0; sample < kSamples; ++sample) {
        for (size_t offset = 0; offset < CaseCount; ++offset) {
            const size_t index = (round + offset) % CaseCount;
            Measurement measurement =
                cases[index].runner(producerCount, consumerCount);
            if (!validMeasurement(measurement)) {
                std::cout << cases[index].name
                          << " sample_failed topology=" << topology
                          << " sample=" << sample << '\n';
                return false;
            }
            samples[index].push_back(measurement);
        }
        ++round;
    }

    for (size_t index = 0; index < CaseCount; ++index) {
        printSummary(cases[index].name,
                     topology,
                     producerCount,
                     consumerCount,
                     cases[index].blockSize,
                     cases[index].emptyCounterThreshold,
                     cases[index].initialIndexSize,
                     cases[index].implicitProducerHashSize,
                     cases[index].consumerRotationQuota,
                     samples[index]);
    }
    return true;
}

bool runComparison(const char* topology, int producerCount, int consumerCount)
{
    using CurrentTraits = galay::mpmc::detail::UnboundedQueueTraits;
    const std::array<TraitCase, 4> cases = {{
        {"moody_raw_default",
         DefaultQueueTraits::BLOCK_SIZE,
         DefaultQueueTraits::EXPLICIT_BLOCK_EMPTY_COUNTER_THRESHOLD,
         DefaultQueueTraits::EXPLICIT_INITIAL_INDEX_SIZE,
         DefaultQueueTraits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE,
         DefaultQueueTraits::EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE,
         &runRawQueue<DefaultQueueTraits>},
        {"moody_raw_current",
         CurrentTraits::BLOCK_SIZE,
         CurrentTraits::EXPLICIT_BLOCK_EMPTY_COUNTER_THRESHOLD,
         CurrentTraits::EXPLICIT_INITIAL_INDEX_SIZE,
         CurrentTraits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE,
         CurrentTraits::EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE,
         &runRawQueue<CurrentTraits>},
        {"mpmc_raw_current",
         CurrentTraits::BLOCK_SIZE,
         CurrentTraits::EXPLICIT_BLOCK_EMPTY_COUNTER_THRESHOLD,
         CurrentTraits::EXPLICIT_INITIAL_INDEX_SIZE,
         CurrentTraits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE,
         CurrentTraits::EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE,
         &runChannel<ChannelPath::kRaw>},
        {"mpmc_token",
         CurrentTraits::BLOCK_SIZE,
         CurrentTraits::EXPLICIT_BLOCK_EMPTY_COUNTER_THRESHOLD,
         CurrentTraits::EXPLICIT_INITIAL_INDEX_SIZE,
         CurrentTraits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE,
         CurrentTraits::EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE,
         &runChannel<ChannelPath::kToken>},
    }};
    std::array<std::vector<Measurement>, 4> samples;
    if (!runCases(topology,
                  producerCount,
                  consumerCount,
                  cases,
                  samples)) {
        return false;
    }
    bool ok = printRatioSummary(
        "ratio_raw_current_over_default", topology, samples[1], samples[0]);
    ok = printRatioSummary(
             "ratio_embedded_raw_over_raw_current",
             topology,
             samples[2],
             samples[1]) &&
        ok;
    ok = printRatioSummary(
             "ratio_token_over_embedded_raw", topology, samples[3], samples[2]) &&
        ok;
    ok = printRatioSummary(
             "ratio_token_over_default_raw", topology, samples[3], samples[0]) &&
        ok;
    return ok;
}

} // namespace

int main()
{
    bool ok = runComparison("2p2c", 2, 2);
    ok = runComparison("4p4c", 4, 4) && ok;
    return ok ? 0 : 1;
}
