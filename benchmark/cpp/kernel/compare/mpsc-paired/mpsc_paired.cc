/**
 * @file mpsc_paired.cc
 * @brief 与 Rust Crossbeam 使用同一协议测量 bounded/unbounded MPSC。
 */

#include <galay/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include <galay/cpp/galay-utils/common/defn.hpp>
#include "benchmark/cpp/common/benchmark_affinity.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

enum class CaseKind : uint8_t { kBounded, kUnbounded };
enum class SendResult : uint8_t { kSent, kRetry, kFailed };
enum class ArgumentError : uint8_t {
    kMissingValue,
    kUnknownOption,
    kInvalidCase,
    kInvalidNumber,
    kInvalidCapacity,
    kInvalidTopology,
    kInvalidMessageRange,
};

struct Config
{
    uint64_t messages = 1'000'000;
    size_t capacity = 4096;
    size_t producers = 1;
    size_t producerCore = 0;
    size_t consumerCore = 1;
    CaseKind kind = CaseKind::kBounded;
};

struct Measurement
{
    double messagesPerSecond = 0.0;
    uint64_t elapsedNs = 0;
    uint64_t received = 0;
    uint64_t checksum = 0;
    uint64_t expectedChecksum = 0;
    uint64_t fullRetries = 0;
    uint64_t emptyRetries = 0;
    galay::benchmark::ThreadPlacement producerPlacement =
        galay::benchmark::ThreadPlacement::kUnsupported;
    galay::benchmark::ThreadPlacement consumerPlacement =
        galay::benchmark::ThreadPlacement::kUnsupported;
    bool fifoOk = false;
    bool sendOk = false;
};

struct alignas(galay::utils::kCacheLineSize) StartState
{
    std::atomic<size_t> ready{0};
    std::atomic<size_t> producersDone{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
};

const char* argumentErrorName(ArgumentError error) noexcept
{
    switch (error) {
    case ArgumentError::kMissingValue: return "missing option value";
    case ArgumentError::kUnknownOption: return "unknown option";
    case ArgumentError::kInvalidCase: return "invalid case";
    case ArgumentError::kInvalidNumber: return "invalid unsigned integer";
    case ArgumentError::kInvalidCapacity:
        return "bounded capacity must be a power of two and at least 2";
    case ArgumentError::kInvalidTopology:
        return "producer count and core placement must describe an MPSC topology";
    case ArgumentError::kInvalidMessageRange:
        return "each producer must emit at most 2^32 messages";
    }
    return "unknown argument error";
}

const char* caseName(CaseKind kind) noexcept
{
    return kind == CaseKind::kBounded ? "bounded" : "unbounded";
}

template <typename UInt>
std::expected<UInt, ArgumentError> parseUnsigned(std::string_view text) noexcept
{
    UInt value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(ArgumentError::kInvalidNumber);
    }
    return value;
}

std::expected<Config, ArgumentError> parseArguments(int argc, char** argv) noexcept
{
    Config config;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            return std::unexpected(ArgumentError::kMissingValue);
        }
        const std::string_view option(argv[index]);
        const std::string_view value(argv[index + 1]);
        if (option == "--case") {
            if (value == "bounded") {
                config.kind = CaseKind::kBounded;
            } else if (value == "unbounded") {
                config.kind = CaseKind::kUnbounded;
            } else {
                return std::unexpected(ArgumentError::kInvalidCase);
            }
        } else if (option == "--messages") {
            auto parsed = parseUnsigned<uint64_t>(value);
            if (!parsed || *parsed == 0) {
                return std::unexpected(ArgumentError::kInvalidNumber);
            }
            config.messages = *parsed;
        } else if (option == "--capacity") {
            auto parsed = parseUnsigned<size_t>(value);
            if (!parsed) return std::unexpected(parsed.error());
            config.capacity = *parsed;
        } else if (option == "--producers") {
            auto parsed = parseUnsigned<size_t>(value);
            if (!parsed || *parsed == 0 || *parsed > 32) {
                return std::unexpected(ArgumentError::kInvalidTopology);
            }
            config.producers = *parsed;
        } else if (option == "--producer-core") {
            auto parsed = parseUnsigned<size_t>(value);
            if (!parsed) return std::unexpected(parsed.error());
            config.producerCore = *parsed;
        } else if (option == "--consumer-core") {
            auto parsed = parseUnsigned<size_t>(value);
            if (!parsed) return std::unexpected(parsed.error());
            config.consumerCore = *parsed;
        } else {
            return std::unexpected(ArgumentError::kUnknownOption);
        }
    }

    if (config.kind == CaseKind::kBounded &&
        (config.capacity < 2 || (config.capacity & (config.capacity - 1)) != 0)) {
        return std::unexpected(ArgumentError::kInvalidCapacity);
    }
    if (config.producerCore >
        std::numeric_limits<size_t>::max() - config.producers) {
        return std::unexpected(ArgumentError::kInvalidTopology);
    }
    if (config.consumerCore >= config.producerCore &&
        config.consumerCore < config.producerCore + config.producers) {
        return std::unexpected(ArgumentError::kInvalidTopology);
    }
    constexpr uint64_t kSequenceCardinality = uint64_t{1} << 32U;
    const uint64_t largestProducerPartition =
        config.messages / config.producers +
        (config.messages % config.producers != 0 ? 1U : 0U);
    if (largestProducerPartition > kSequenceCardinality) {
        return std::unexpected(ArgumentError::kInvalidMessageRange);
    }
    return config;
}

uint64_t firstSequence(const Config& config, size_t producer) noexcept
{
    return config.messages * producer / config.producers;
}

uint64_t producerMessages(const Config& config, size_t producer) noexcept
{
    return firstSequence(config, producer + 1) - firstSequence(config, producer);
}

uint64_t triangularChecksum(uint64_t count) noexcept
{
    return (count & 1U) == 0
        ? (count / 2) * (count - 1)
        : count * ((count - 1) / 2);
}

uint64_t expectedChecksum(const Config& config) noexcept
{
    uint64_t checksum = 0;
    for (size_t producer = 0; producer < config.producers; ++producer) {
        const uint64_t count = producerMessages(config, producer);
        checksum += (static_cast<uint64_t>(producer) << 32U) * count;
        checksum += triangularChecksum(count);
    }
    return checksum;
}

uint64_t encodeValue(size_t producer, uint64_t sequence) noexcept
{
    return (static_cast<uint64_t>(producer) << 32U) | sequence;
}

template <typename Send, typename Receive>
Measurement runMpsc(const Config& config, Send&& send, Receive&& receive)
{
    StartState state;
    std::vector<uint64_t> fullRetries(config.producers, 0);
    std::vector<galay::benchmark::ThreadPlacement> producerPlacements(
        config.producers, galay::benchmark::ThreadPlacement::kUnsupported);
    std::vector<std::thread> producers;
    producers.reserve(config.producers);
    uint64_t emptyRetries = 0;
    uint64_t received = 0;
    uint64_t checksum = 0;
    bool fifoOk = true;
    auto consumerPlacement = galay::benchmark::ThreadPlacement::kUnsupported;

    for (size_t producer = 0; producer < config.producers; ++producer) {
        producers.emplace_back([&, producer]() {
            producerPlacements[producer] = galay::benchmark::pinCurrentThread(
                config.producerCore + producer);
            const size_t ready = state.ready.fetch_add(1, std::memory_order_release) + 1;
            if (ready > config.producers + 1) {
                state.failed.store(true, std::memory_order_release);
            }
            while (!state.start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const uint64_t count = producerMessages(config, producer);
            uint64_t retries = 0;
            bool sendOk = true;
            for (uint64_t sequence = 0; sequence < count; ++sequence) {
                uint64_t pending = encodeValue(producer, sequence);
                for (;;) {
                    const SendResult result = send(producer, pending);
                    if (result == SendResult::kSent) break;
                    if (result == SendResult::kFailed) {
                        sendOk = false;
                        state.failed.store(true, std::memory_order_release);
                        break;
                    }
                    ++retries;
                    std::this_thread::yield();
                }
                if (!sendOk) break;
            }
            fullRetries[producer] = retries;
            const size_t done =
                state.producersDone.fetch_add(1, std::memory_order_release) + 1;
            if (done > config.producers) {
                state.failed.store(true, std::memory_order_release);
            }
        });
    }

    std::thread consumer([&]() {
        consumerPlacement = galay::benchmark::pinCurrentThread(config.consumerCore);
        const size_t ready = state.ready.fetch_add(1, std::memory_order_release) + 1;
        if (ready > config.producers + 1) {
            state.failed.store(true, std::memory_order_release);
        }
        while (!state.start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::vector<uint64_t> expected(config.producers, 0);
        bool allProducersDone = false;
        while (received < config.messages &&
               !state.failed.load(std::memory_order_acquire)) {
            auto value = receive();
            if (!value.has_value()) {
                ++emptyRetries;
                if (allProducersDone) {
                    break;
                }
                allProducersDone =
                    state.producersDone.load(std::memory_order_acquire) ==
                    config.producers;
                if (!allProducersDone) {
                    std::this_thread::yield();
                }
                continue;
            }
            const size_t producer = static_cast<size_t>(*value >> 32U);
            const uint64_t sequence = *value & 0xffff'ffffULL;
            if (producer >= config.producers || sequence != expected[producer]) {
                fifoOk = false;
                state.failed.store(true, std::memory_order_release);
                break;
            }
            ++expected[producer];
            checksum += *value;
            ++received;
        }
        for (size_t producer = 0; producer < config.producers; ++producer) {
            fifoOk = fifoOk && expected[producer] == producerMessages(config, producer);
        }
    });

    while (state.ready.load(std::memory_order_acquire) != config.producers + 1) {
        std::this_thread::yield();
    }
    const auto begin = std::chrono::steady_clock::now();
    state.start.store(true, std::memory_order_release);
    for (std::thread& producer : producers) producer.join();
    consumer.join();
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
    auto producerPlacement = galay::benchmark::ThreadPlacement::kPinnedToCore;
    for (const auto placement : producerPlacements) {
        producerPlacement = std::max(producerPlacement, placement);
    }
    const uint64_t elapsed = elapsedNs > 0 ? static_cast<uint64_t>(elapsedNs) : 0;
    return {
        .messagesPerSecond = elapsed > 0
            ? static_cast<double>(config.messages) * 1'000'000'000.0 / elapsed
            : 0.0,
        .elapsedNs = elapsed,
        .received = received,
        .checksum = checksum,
        .expectedChecksum = expectedChecksum(config),
        .fullRetries = std::accumulate(fullRetries.begin(), fullRetries.end(), uint64_t{0}),
        .emptyRetries = emptyRetries,
        .producerPlacement = producerPlacement,
        .consumerPlacement = consumerPlacement,
        .fifoOk = fifoOk,
        .sendOk = !state.failed.load(std::memory_order_acquire),
    };
}

Measurement runBounded(const Config& config)
{
    galay::mpsc::BoundedChannel<uint64_t> channel(config.capacity);
    return runMpsc(
        config,
        [&channel](size_t, uint64_t& value) {
            return channel.trySend(std::move(value))
                ? SendResult::kSent : SendResult::kRetry;
        },
        [&channel]() { return channel.tryRecv(); });
}

Measurement runUnbounded(const Config& config)
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    Channel channel;
    std::vector<Channel::ProducerToken> tokens;
    tokens.reserve(config.producers);
    for (size_t producer = 0; producer < config.producers; ++producer) {
        auto token = channel.makeProducerToken();
        if (!token.valid()) return {};
        tokens.push_back(std::move(token));
    }
    return runMpsc(
        config,
        [&channel, &tokens](size_t producer, uint64_t& value) {
            return channel.send(tokens[producer], std::move(value))
                ? SendResult::kSent : SendResult::kFailed;
        },
        [&channel]() { return channel.tryRecv(); });
}

} // namespace

int main(int argc, char** argv)
{
    auto config = parseArguments(argc, argv);
    if (!config) {
        std::cerr << "mpsc paired benchmark argument error: "
                  << argumentErrorName(config.error()) << '\n';
        return std::cerr.good() ? 2 : 3;
    }
    const Measurement measurement = config->kind == CaseKind::kBounded
        ? runBounded(*config) : runUnbounded(*config);
    const bool valid = measurement.elapsedNs > 0 && measurement.sendOk &&
        measurement.received == config->messages && measurement.fifoOk &&
        measurement.checksum == measurement.expectedChecksum;
    const size_t reportedCapacity =
        config->kind == CaseKind::kBounded ? config->capacity : 0;

    std::cout << std::setprecision(17) << std::boolalpha
              << "{\"schema\":\"galay.mpsc.paired.v1\""
              << ",\"language\":\"cpp\""
              << ",\"case\":\"" << caseName(config->kind) << "\""
              << ",\"topology\":\"" << config->producers << "p1c\""
              << ",\"ordering\":\"producer_fifo\""
              << ",\"payload_bytes\":8"
              << ",\"capacity\":" << reportedCapacity
              << ",\"messages\":" << config->messages
              << ",\"elapsed_ns\":" << measurement.elapsedNs
              << ",\"messages_per_second\":" << measurement.messagesPerSecond
              << ",\"received\":" << measurement.received
              << ",\"checksum\":" << measurement.checksum
              << ",\"expected_checksum\":" << measurement.expectedChecksum
              << ",\"fifo_ok\":" << measurement.fifoOk
              << ",\"full_retries\":" << measurement.fullRetries
              << ",\"empty_retries\":" << measurement.emptyRetries
              << ",\"producer_placement\":\""
              << galay::benchmark::threadPlacementName(measurement.producerPlacement) << "\""
              << ",\"consumer_placement\":\""
              << galay::benchmark::threadPlacementName(measurement.consumerPlacement) << "\""
              << ",\"backoff\":\"yield\""
              << ",\"generator\":\"per_producer_monotonic_u64\""
              << ",\"valid\":" << valid << "}\n";
    if (!std::cout.good()) return 3;
    return valid ? 0 : 1;
}
