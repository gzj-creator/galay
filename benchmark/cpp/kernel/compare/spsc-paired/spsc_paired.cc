/**
 * @file spsc_paired.cc
 * @brief 与 Rust 使用同一协议测量 bounded/unbounded SPSC 1P1C 吞吐。
 */

#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>
#include "benchmark/cpp/common/benchmark_affinity.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace {

enum class CaseKind : uint8_t {
    kRawBounded,
    kUnbounded,
};

enum class ArgumentError : uint8_t {
    kMissingValue,
    kUnknownOption,
    kInvalidCase,
    kInvalidNumber,
    kInvalidCapacity,
    kInvalidCorePair,
};

struct Config {
    uint64_t messages = 1'000'000;
    size_t capacity = 4096;
    size_t producerCore = 0;
    size_t consumerCore = 1;
    CaseKind kind = CaseKind::kRawBounded;
};

struct Measurement {
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

struct alignas(128) StartState {
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
};

struct alignas(128) ProducerResult {
    uint64_t fullRetries = 0;
    galay::benchmark::ThreadPlacement placement =
        galay::benchmark::ThreadPlacement::kUnsupported;
    bool readyOk = true;
};

struct alignas(128) ConsumerResult {
    uint64_t emptyRetries = 0;
    uint64_t received = 0;
    uint64_t checksum = 0;
    galay::benchmark::ThreadPlacement placement =
        galay::benchmark::ThreadPlacement::kUnsupported;
    bool fifoOk = true;
    bool readyOk = true;
};

const char* argumentErrorName(ArgumentError error) noexcept
{
    switch (error) {
    case ArgumentError::kMissingValue:
        return "missing option value";
    case ArgumentError::kUnknownOption:
        return "unknown option";
    case ArgumentError::kInvalidCase:
        return "invalid case";
    case ArgumentError::kInvalidNumber:
        return "invalid unsigned integer";
    case ArgumentError::kInvalidCapacity:
        return "bounded capacity must be a power of two and at least 2";
    case ArgumentError::kInvalidCorePair:
        return "producer and consumer cores must differ";
    }
    return "unknown argument error";
}

const char* caseName(CaseKind kind) noexcept
{
    switch (kind) {
    case CaseKind::kRawBounded:
        return "raw_bounded";
    case CaseKind::kUnbounded:
        return "unbounded";
    }
    return "unknown";
}

template <typename UInt>
std::expected<UInt, ArgumentError> parseUnsigned(std::string_view text) noexcept
{
    UInt value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
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
            if (value == "raw_bounded") {
                config.kind = CaseKind::kRawBounded;
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
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            config.capacity = *parsed;
        } else if (option == "--producer-core") {
            auto parsed = parseUnsigned<size_t>(value);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            config.producerCore = *parsed;
        } else if (option == "--consumer-core") {
            auto parsed = parseUnsigned<size_t>(value);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            config.consumerCore = *parsed;
        } else {
            return std::unexpected(ArgumentError::kUnknownOption);
        }
    }

    if (config.kind == CaseKind::kRawBounded &&
        (config.capacity < 2 || (config.capacity & (config.capacity - 1)) != 0)) {
        return std::unexpected(ArgumentError::kInvalidCapacity);
    }
    if (config.producerCore == config.consumerCore) {
        return std::unexpected(ArgumentError::kInvalidCorePair);
    }
    return config;
}

uint64_t expectedChecksum(uint64_t messages) noexcept
{
    // Divide the even factor first; unsigned multiplication then gives the
    // exact triangular checksum modulo 2^64 without a non-portable wide type.
    return (messages & 1U) == 0
        ? (messages / 2) * (messages - 1)
        : messages * ((messages - 1) / 2);
}

template <typename Send, typename Receive>
Measurement runPair(const Config& config, Send&& send, Receive&& receive)
{
    StartState state;
    ProducerResult producerResult;
    ConsumerResult consumerResult;

    std::thread producer([&]() {
        producerResult.placement =
            galay::benchmark::pinCurrentThread(config.producerCore);
        const size_t readyCount =
            state.ready.fetch_add(1, std::memory_order_release) + 1;
        producerResult.readyOk = readyCount <= 2;
        while (!state.start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (uint64_t sequence = 0; sequence < config.messages; ++sequence) {
            uint64_t pending = sequence;
            while (!send(pending)) {
                ++producerResult.fullRetries;
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        consumerResult.placement =
            galay::benchmark::pinCurrentThread(config.consumerCore);
        const size_t readyCount =
            state.ready.fetch_add(1, std::memory_order_release) + 1;
        consumerResult.readyOk = readyCount <= 2;
        while (!state.start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (consumerResult.received < config.messages) {
            uint64_t value = 0;
            if (receive(value)) {
                consumerResult.fifoOk =
                    consumerResult.fifoOk && value == consumerResult.received;
                consumerResult.checksum += value;
                ++consumerResult.received;
                continue;
            }
            ++consumerResult.emptyRetries;
            // 只按预期数量结束，避免 producer 完成后的瞬时空读造成伪失败。
            std::this_thread::yield();
        }
    });

    while (state.ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    const auto begin = std::chrono::steady_clock::now();
    state.start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto elapsedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const uint64_t elapsedValue = elapsedNs > 0 ? static_cast<uint64_t>(elapsedNs) : 0;
    const uint64_t expected = expectedChecksum(config.messages);
    const bool sendOk = producerResult.readyOk &&
        (producerResult.fullRetries == 0 || config.kind == CaseKind::kRawBounded);

    return {
        .messagesPerSecond = elapsedValue > 0
            ? static_cast<double>(config.messages) * 1'000'000'000.0 /
                static_cast<double>(elapsedValue)
            : 0.0,
        .elapsedNs = elapsedValue,
        .received = consumerResult.received,
        .checksum = consumerResult.checksum,
        .expectedChecksum = expected,
        .fullRetries = producerResult.fullRetries,
        .emptyRetries = consumerResult.emptyRetries,
        .producerPlacement = producerResult.placement,
        .consumerPlacement = consumerResult.placement,
        .fifoOk = consumerResult.readyOk && consumerResult.fifoOk,
        .sendOk = sendOk,
    };
}

Measurement runBounded(const Config& config)
{
    galay::spsc::Ring<uint64_t> channel(config.capacity);
    if (channel.error() != galay::spsc::RingError::kNone) {
        return {};
    }
    return runPair(
        config,
        [&channel](uint64_t& value) { return channel.trySend(std::move(value)); },
        [&channel](uint64_t& value) { return channel.tryRecv(value); });
}

Measurement runUnbounded(const Config& config)
{
    galay::spsc::UnboundedQueue<uint64_t> channel;
    if (!channel.valid()) {
        return {};
    }
    return runPair(
        config,
        [&channel](uint64_t& value) { return channel.send(std::move(value)); },
        [&channel](uint64_t& value) { return channel.tryRecv(value); });
}

} // namespace

int main(int argc, char** argv)
{
    auto config = parseArguments(argc, argv);
    if (!config) {
        std::cerr << "spsc paired benchmark argument error: "
                  << argumentErrorName(config.error()) << '\n';
        if (!std::cerr.good()) {
            return 3;
        }
        return 2;
    }

    const Measurement measurement = config->kind == CaseKind::kRawBounded
        ? runBounded(*config)
        : runUnbounded(*config);
    const bool valid = measurement.elapsedNs > 0 && measurement.sendOk &&
        measurement.received == config->messages && measurement.fifoOk &&
        measurement.checksum == measurement.expectedChecksum;
    const size_t reportedCapacity =
        config->kind == CaseKind::kRawBounded ? config->capacity : 0;

    std::cout << std::setprecision(17) << std::boolalpha
              << "{\"schema\":\"galay.spsc.paired.v1\""
              << ",\"language\":\"cpp\""
              << ",\"case\":\"" << caseName(config->kind) << "\""
              << ",\"topology\":\"1p1c\""
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
              << ",\"generator\":\"monotonic_u64\""
              << ",\"valid\":" << valid << "}\n";
    if (!std::cout.good()) {
        return 3;
    }
    return valid ? 0 : 1;
}
