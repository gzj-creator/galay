/**
 * @file b27_spsc_static_ring_throughput.cc
 * @brief 在相同严格 1P1C harness 中比较动态与编译期容量 BoundedChannel。
 *
 * @details 每轮按 dynamic/static/static/dynamic 的 ABBA 顺序执行，生产者发送
 * 单调序列，消费者只在取得预期数量后退出，并校验 FIFO、全部 payload word、
 * 消息数量和 checksum。构造、线程创建和起跑同步不计入样本时间。
 */

#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>
#include "benchmark/cpp/common/benchmark_affinity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct Config
{
    uint64_t messages = 400'000'000;
    uint64_t warmupMessages = 2'000'000;
    size_t rounds = 3;
    size_t capacity = 0;   // 0 表示 256 与 4096。
    size_t payloadBytes = 0; // 0 表示 8B 与 64B。
};

enum class ParseError : uint8_t {
    kMissingValue,
    kInvalidNumber,
    kInvalidValue,
    kUnknownOption,
};

[[nodiscard]] const char* parseErrorName(ParseError error) noexcept
{
    switch (error) {
    case ParseError::kMissingValue:
        return "missing option value";
    case ParseError::kInvalidNumber:
        return "invalid unsigned integer";
    case ParseError::kInvalidValue:
        return "unsupported option value";
    case ParseError::kUnknownOption:
        return "unknown option";
    }
    return "unknown parse error";
}

[[nodiscard]] std::expected<uint64_t, ParseError>
parseUnsigned(std::string_view text) noexcept
{
    uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::unexpected(ParseError::kInvalidNumber);
    }
    return value;
}

[[nodiscard]] std::expected<Config, ParseError>
parseArguments(int argc, char** argv) noexcept
{
    Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (index + 1 >= argc) {
            return std::unexpected(ParseError::kMissingValue);
        }
        const auto parsed = parseUnsigned(argv[++index]);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }

        if (option == "--messages") {
            if (*parsed == 0) {
                return std::unexpected(ParseError::kInvalidValue);
            }
            config.messages = *parsed;
        } else if (option == "--warmup-messages") {
            config.warmupMessages = *parsed;
        } else if (option == "--rounds") {
            if (*parsed == 0 || *parsed > std::numeric_limits<size_t>::max()) {
                return std::unexpected(ParseError::kInvalidValue);
            }
            config.rounds = static_cast<size_t>(*parsed);
        } else if (option == "--capacity") {
            if (*parsed != 256 && *parsed != 4096) {
                return std::unexpected(ParseError::kInvalidValue);
            }
            config.capacity = static_cast<size_t>(*parsed);
        } else if (option == "--payload-bytes") {
            if (*parsed != 8 && *parsed != 64) {
                return std::unexpected(ParseError::kInvalidValue);
            }
            config.payloadBytes = static_cast<size_t>(*parsed);
        } else {
            return std::unexpected(ParseError::kUnknownOption);
        }
    }
    return config;
}

template <size_t Words>
using Payload = std::conditional_t<Words == 1,
                                   uint64_t,
                                   std::array<uint64_t, Words>>;

static_assert(std::is_same_v<Payload<1>, uint64_t>);
static_assert(sizeof(Payload<1>) == 8);
static_assert(sizeof(Payload<8>) == 64);

template <size_t Words>
[[nodiscard]] Payload<Words> makePayload(uint64_t sequence) noexcept
{
    if constexpr (Words == 1) {
        return sequence;
    } else {
        Payload<Words> payload{};
        for (size_t word = 0; word < Words; ++word) {
            payload[word] = sequence + static_cast<uint64_t>(word);
        }
        return payload;
    }
}

[[nodiscard]] uint64_t triangularChecksum(uint64_t count) noexcept
{
    return (count & 1U) == 0
        ? (count / 2) * (count - 1)
        : count * ((count - 1) / 2);
}

template <size_t Words>
[[nodiscard]] uint64_t expectedChecksum(uint64_t messages) noexcept
{
    constexpr uint64_t kWordOffsetSum = Words * (Words - 1) / 2;
    return static_cast<uint64_t>(Words) * triangularChecksum(messages) +
        messages * kWordOffsetSum;
}

struct Measurement
{
    double messagesPerSecond = 0.0;
    uint64_t elapsedNs = 0;
    uint64_t received = 0;
    uint64_t checksum = 0;
    uint64_t expected = 0;
    uint64_t fullRetries = 0;
    uint64_t emptyRetries = 0;
    galay::benchmark::ThreadPlacement producerPlacement =
        galay::benchmark::ThreadPlacement::kUnsupported;
    galay::benchmark::ThreadPlacement consumerPlacement =
        galay::benchmark::ThreadPlacement::kUnsupported;
    bool fifoOk = true;
    bool contentOk = true;
    bool readyOk = true;
    bool extraMessage = false;
    bool valid = false;
};

template <size_t Words, typename Channel>
[[nodiscard]] Measurement runPair(Channel& channel, uint64_t messages)
{
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    Measurement measurement;
    bool producerReadyOk = false;
    bool consumerReadyOk = false;

    std::thread producer([&]() {
        measurement.producerPlacement = galay::benchmark::pinCurrentThread(0);
        const size_t readyCount = ready.fetch_add(1, std::memory_order_release) + 1;
        producerReadyOk = readyCount <= 2;
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        uint64_t retries = 0;
        for (uint64_t sequence = 0; sequence < messages; ++sequence) {
            Payload<Words> pending = makePayload<Words>(sequence);
            while (!channel.trySend(std::move(pending))) {
                ++retries;
                std::this_thread::yield();
            }
        }
        measurement.fullRetries = retries;
    });

    std::thread consumer([&]() {
        measurement.consumerPlacement = galay::benchmark::pinCurrentThread(1);
        const size_t readyCount = ready.fetch_add(1, std::memory_order_release) + 1;
        consumerReadyOk = readyCount <= 2;
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        uint64_t retries = 0;
        uint64_t received = 0;
        uint64_t checksum = 0;
        bool fifoOk = true;
        bool contentOk = true;
        while (received < messages) {
            auto value = channel.tryRecv();
            if (!value.has_value()) {
                ++retries;
                std::this_thread::yield();
                continue;
            }
            if constexpr (Words == 1) {
                fifoOk = fifoOk && *value == received;
                contentOk = contentOk && *value == received;
                checksum += *value;
            } else {
                fifoOk = fifoOk && (*value)[0] == received;
                for (size_t word = 0; word < Words; ++word) {
                    const uint64_t expectedWord =
                        received + static_cast<uint64_t>(word);
                    contentOk = contentOk && (*value)[word] == expectedWord;
                    checksum += (*value)[word];
                }
            }
            ++received;
        }
        measurement.received = received;
        measurement.checksum = checksum;
        measurement.emptyRetries = retries;
        measurement.fifoOk = fifoOk;
        measurement.contentOk = contentOk;
    });

    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto elapsedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    measurement.extraMessage = channel.tryRecv().has_value();
    measurement.elapsedNs =
        elapsedNs > 0 ? static_cast<uint64_t>(elapsedNs) : 0;
    measurement.readyOk = producerReadyOk && consumerReadyOk;
    measurement.expected = expectedChecksum<Words>(messages);
    measurement.messagesPerSecond = measurement.elapsedNs > 0
        ? static_cast<double>(messages) * 1'000'000'000.0 /
            static_cast<double>(measurement.elapsedNs)
        : 0.0;
    measurement.valid = measurement.elapsedNs > 0 && measurement.readyOk &&
        measurement.received == messages && measurement.fifoOk &&
        measurement.contentOk && measurement.checksum == measurement.expected &&
        !measurement.extraMessage;
    return measurement;
}

template <size_t Words, size_t Capacity>
[[nodiscard]] Measurement runDynamic(uint64_t messages)
{
    galay::spsc::BoundedChannel<Payload<Words>> channel(Capacity);
    if (channel.error() != galay::spsc::RingError::kNone) {
        return {};
    }
    return runPair<Words>(channel, messages);
}

template <size_t Words, size_t Capacity>
[[nodiscard]] Measurement runStatic(uint64_t messages)
{
    using Channel = galay::spsc::BoundedChannel<Payload<Words>, Capacity>;
    // Capacity=4096 and a 64-byte payload make the inline slots hundreds of
    // KiB. Keep that benchmark fixture off the stack; allocation remains
    // outside runPair's timed interval.
    std::unique_ptr<Channel> channel(new (std::nothrow) Channel());
    if (!channel || channel->error() != galay::spsc::RingError::kNone) {
        return {};
    }
    return runPair<Words>(*channel, messages);
}

[[nodiscard]] double median(const std::vector<double>& samples)
{
    if (samples.empty()) {
        return 0.0;
    }
    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    const size_t middle = sorted.size() / 2;
    return (sorted.size() & 1U) != 0
        ? sorted[middle]
        : (sorted[middle - 1] + sorted[middle]) / 2.0;
}

[[nodiscard]] double coefficientOfVariation(const std::vector<double>& samples)
{
    if (samples.size() < 2) {
        return 0.0;
    }
    double sum = 0.0;
    for (const double sample : samples) {
        sum += sample;
    }
    const double mean = sum / static_cast<double>(samples.size());
    if (mean == 0.0) {
        return 0.0;
    }
    double squaredDifference = 0.0;
    for (const double sample : samples) {
        const double difference = sample - mean;
        squaredDifference += difference * difference;
    }
    const double sampleDeviation = std::sqrt(
        squaredDifference / static_cast<double>(samples.size() - 1));
    return sampleDeviation / mean * 100.0;
}

template <size_t Words, size_t Capacity>
[[nodiscard]] bool runCase(const Config& config)
{
    const std::array warmups{
        runDynamic<Words, Capacity>(config.warmupMessages),
        runStatic<Words, Capacity>(config.warmupMessages),
        runStatic<Words, Capacity>(config.warmupMessages),
        runDynamic<Words, Capacity>(config.warmupMessages),
    };
    for (const Measurement& warmup : warmups) {
        if (!warmup.valid) {
            std::cout << "warmup_failed capacity=" << Capacity
                      << " payload_bytes=" << sizeof(Payload<Words>) << '\n';
            return false;
        }
    }

    std::vector<double> dynamicSamples;
    std::vector<double> staticSamples;
    std::vector<double> pairedRatios;
    dynamicSamples.reserve(config.rounds * 2);
    staticSamples.reserve(config.rounds * 2);
    pairedRatios.reserve(config.rounds);

    for (size_t round = 0; round < config.rounds; ++round) {
        const Measurement dynamicFirst =
            runDynamic<Words, Capacity>(config.messages);
        const Measurement staticFirst =
            runStatic<Words, Capacity>(config.messages);
        const Measurement staticSecond =
            runStatic<Words, Capacity>(config.messages);
        const Measurement dynamicSecond =
            runDynamic<Words, Capacity>(config.messages);
        const std::array samples{
            std::pair{"dynamic", &dynamicFirst},
            std::pair{"static", &staticFirst},
            std::pair{"static", &staticSecond},
            std::pair{"dynamic", &dynamicSecond},
        };

        for (size_t order = 0; order < samples.size(); ++order) {
            const auto& [variant, sample] = samples[order];
            std::cout << std::fixed << std::setprecision(3)
                      << "sample capacity=" << Capacity
                      << " payload_bytes=" << sizeof(Payload<Words>)
                      << " round=" << round
                      << " order=" << order
                      << " variant=" << variant
                      << " messages=" << config.messages
                      << " seconds=" << static_cast<double>(sample->elapsedNs) / 1e9
                      << " msg_s=" << sample->messagesPerSecond
                      << " full_retries=" << sample->fullRetries
                      << " empty_retries=" << sample->emptyRetries
                      << " producer_placement="
                      << galay::benchmark::threadPlacementName(
                             sample->producerPlacement)
                      << " consumer_placement="
                      << galay::benchmark::threadPlacementName(
                             sample->consumerPlacement)
                      << " valid=" << std::boolalpha << sample->valid << '\n';
            if (!sample->valid) {
                return false;
            }
        }

        dynamicSamples.push_back(dynamicFirst.messagesPerSecond);
        dynamicSamples.push_back(dynamicSecond.messagesPerSecond);
        staticSamples.push_back(staticFirst.messagesPerSecond);
        staticSamples.push_back(staticSecond.messagesPerSecond);
        const double ratio = std::sqrt(
            (staticFirst.messagesPerSecond * staticSecond.messagesPerSecond) /
            (dynamicFirst.messagesPerSecond * dynamicSecond.messagesPerSecond));
        pairedRatios.push_back(ratio);
    }

    const double dynamicMedian = median(dynamicSamples);
    const double staticMedian = median(staticSamples);
    std::cout << std::fixed << std::setprecision(3)
              << "summary capacity=" << Capacity
              << " payload_bytes=" << sizeof(Payload<Words>)
              << " samples_per_variant=" << dynamicSamples.size()
              << " dynamic_median_msg_s=" << dynamicMedian
              << " dynamic_cv_pct=" << coefficientOfVariation(dynamicSamples)
              << " static_median_msg_s=" << staticMedian
              << " static_cv_pct=" << coefficientOfVariation(staticSamples)
              << " median_throughput_ratio="
              << (dynamicMedian > 0.0 ? staticMedian / dynamicMedian : 0.0)
              << " paired_median_ratio=" << median(pairedRatios)
              << " valid=true\n";
    return std::cout.good();
}

} // namespace

int main(int argc, char** argv)
{
    const auto config = parseArguments(argc, argv);
    if (!config.has_value()) {
        std::cerr << "b27 argument error: " << parseErrorName(config.error()) << '\n';
        return 2;
    }

    bool valid = true;
    if (config->payloadBytes == 0 || config->payloadBytes == 8) {
        if (config->capacity == 0 || config->capacity == 256) {
            valid = runCase<1, 256>(*config) && valid;
        }
        if (config->capacity == 0 || config->capacity == 4096) {
            valid = runCase<1, 4096>(*config) && valid;
        }
    }
    if (config->payloadBytes == 0 || config->payloadBytes == 64) {
        if (config->capacity == 0 || config->capacity == 256) {
            valid = runCase<8, 256>(*config) && valid;
        }
        if (config->capacity == 0 || config->capacity == 4096) {
            valid = runCase<8, 4096>(*config) && valid;
        }
    }
    if (!std::cout.good()) {
        return 3;
    }
    return valid ? 0 : 1;
}
