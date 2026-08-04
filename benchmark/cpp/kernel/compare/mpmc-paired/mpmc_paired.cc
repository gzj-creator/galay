/**
 * @file mpmc_paired.cc
 * @brief 与 Rust Crossbeam 使用同一协议测量 bounded/unbounded MPMC。
 */

#include "benchmark/cpp/common/benchmark_affinity.h"
#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <iostream>
#include <numeric>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace galay::mpmc {

/** @brief 仅供 paired benchmark 隔离 wrapper 与底层数据面。 */
struct UnboundedChannelTestAccess
{
    template <UnboundedValue T>
    static bool rawSend(UnboundedChannel<T>& channel,
                        typename UnboundedChannel<T>::ProducerToken& token, T&& value)
    {
        return token.validFor(channel) &&
            channel.template sendTokenFast<false>(token, std::move(value));
    }

    template <UnboundedValue T>
    static bool rawRecv(UnboundedChannel<T>& channel,
                        typename UnboundedChannel<T>::ConsumerToken& token, T& value)
    {
        auto received = channel.tryRecv(token);
        if (!received.has_value()) {
            return false;
        }
        value = std::move(*received);
        return true;
    }
};

} // namespace galay::mpmc

namespace {

enum class ArgumentError : uint8_t {
    kMissingValue,
    kUnknownOption,
    kInvalidNumber,
    kInvalidTopology,
    kInvalidCase,
    kInvalidPath,
};

enum class Path : uint8_t {
    kToken,
    kRaw,
    kRawSend,
    kRawRecv,
};

enum class ChannelCase : uint8_t {
    kUnbounded,
    kBounded,
};

struct Config
{
    uint64_t messages = 5'000'000;
    size_t producers = 2;
    size_t consumers = 2;
    size_t capacity = 4096;
    ChannelCase channelCase = ChannelCase::kUnbounded;
    Path path = Path::kToken;
};

struct Measurement
{
    uint64_t elapsedNs = 0;
    uint64_t received = 0;
    uint64_t checksum = 0;
    uint64_t sendRetries = 0;
    uint64_t emptyRetries = 0;
    size_t finalSize = 0;
    galay::benchmark::ThreadPlacement placement = galay::benchmark::ThreadPlacement::kUnsupported;
    bool setupFailed = false;
};

const char* argumentErrorName(ArgumentError error) noexcept
{
    switch (error) {
    case ArgumentError::kMissingValue:
        return "missing option value";
    case ArgumentError::kUnknownOption:
        return "unknown option";
    case ArgumentError::kInvalidNumber:
        return "invalid unsigned integer";
    case ArgumentError::kInvalidTopology:
        return "producer and consumer counts must match and be 2 or 4";
    case ArgumentError::kInvalidCase:
        return "case must be bounded or unbounded";
    case ArgumentError::kInvalidPath:
        return "bounded case only supports the public direct path";
    }
    return "unknown argument error";
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
        const std::string_view text(argv[index + 1]);
        if (option == "--messages") {
            auto value = parseUnsigned<uint64_t>(text);
            if (!value || *value == 0 || *value > (uint64_t{1} << 32U)) {
                return std::unexpected(ArgumentError::kInvalidNumber);
            }
            config.messages = *value;
        } else if (option == "--producers") {
            auto value = parseUnsigned<size_t>(text);
            if (!value)
                return std::unexpected(value.error());
            config.producers = *value;
        } else if (option == "--consumers") {
            auto value = parseUnsigned<size_t>(text);
            if (!value)
                return std::unexpected(value.error());
            config.consumers = *value;
        } else if (option == "--capacity") {
            auto value = parseUnsigned<size_t>(text);
            if (!value || *value == 0) {
                return std::unexpected(ArgumentError::kInvalidNumber);
            }
            config.capacity = *value;
        } else if (option == "--case") {
            if (text == "unbounded") {
                config.channelCase = ChannelCase::kUnbounded;
            } else if (text == "bounded") {
                config.channelCase = ChannelCase::kBounded;
            } else {
                return std::unexpected(ArgumentError::kInvalidCase);
            }
        } else if (option == "--path") {
            if (text == "raw") {
                config.path = Path::kRaw;
            } else if (text == "token") {
                config.path = Path::kToken;
            } else if (text == "raw-send") {
                config.path = Path::kRawSend;
            } else if (text == "raw-recv") {
                config.path = Path::kRawRecv;
            } else {
                return std::unexpected(ArgumentError::kInvalidNumber);
            }
        } else {
            return std::unexpected(ArgumentError::kUnknownOption);
        }
    }
    if (config.producers != config.consumers || (config.producers != 2 && config.producers != 4)) {
        return std::unexpected(ArgumentError::kInvalidTopology);
    }
    if (config.channelCase == ChannelCase::kBounded && config.path != Path::kToken) {
        return std::unexpected(ArgumentError::kInvalidPath);
    }
    if (config.channelCase == ChannelCase::kBounded &&
        (config.capacity < 2 || !std::has_single_bit(config.capacity))) {
        return std::unexpected(ArgumentError::kInvalidNumber);
    }
    return config;
}

const char* caseName(ChannelCase channelCase) noexcept
{
    return channelCase == ChannelCase::kBounded ? "bounded" : "unbounded";
}

const char* pathName(Path path) noexcept
{
    switch (path) {
    case Path::kToken:
        return "token";
    case Path::kRaw:
        return "raw";
    case Path::kRawSend:
        return "raw-send";
    case Path::kRawRecv:
        return "raw-recv";
    }
    return "unknown";
}

uint64_t expectedChecksum(uint64_t messages) noexcept
{
    return (messages & 1U) == 0 ? (messages / 2) * (messages - 1) : messages * ((messages - 1) / 2);
}

template <bool RawSend, bool RawRecv> Measurement runPath(const Config& config)
{
    using Channel = galay::mpmc::UnboundedChannel<uint64_t>;
    Channel channel;
    std::atomic<bool> setupFailed{false};
    galay::benchmark::CompletionLatch ready(config.producers + config.consumers);
    galay::benchmark::StartGate start;
    std::vector<uint64_t> sendRetries(config.producers, 0);
    std::vector<uint64_t> emptyRetries(config.consumers, 0);
    std::vector<uint64_t> received(config.consumers, 0);
    std::vector<uint64_t> checksums(config.consumers, 0);
    std::vector<galay::benchmark::ThreadPlacement> placements(
        config.producers + config.consumers, galay::benchmark::ThreadPlacement::kUnsupported);
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(config.producers);
    consumers.reserve(config.consumers);

    for (size_t producer = 0; producer < config.producers; ++producer) {
        producers.emplace_back([&, producer]() {
            placements[producer] = galay::benchmark::pinCurrentThread(producer);
            auto token = channel.makeProducerToken();
            if (!token.valid()) {
                setupFailed.store(true, std::memory_order_release);
            }
            ready.arrive();
            start.wait();
            if (setupFailed.load(std::memory_order_acquire)) {
                return;
            }

            const uint64_t first = config.messages * producer / config.producers;
            const uint64_t last = config.messages * (producer + 1) / config.producers;
            uint64_t retries = 0;
            for (uint64_t id = first; id < last; ++id) {
                uint64_t value = id;
                bool sent = false;
                if constexpr (RawSend) {
                    sent = galay::mpmc::UnboundedChannelTestAccess::rawSend(channel, token,
                                                                            std::move(value));
                } else {
                    sent = channel.send(token, std::move(value));
                }
                while (!sent) {
                    ++retries;
                    std::this_thread::yield();
                    if constexpr (RawSend) {
                        sent = galay::mpmc::UnboundedChannelTestAccess::rawSend(channel, token,
                                                                                std::move(value));
                    } else {
                        sent = channel.send(token, std::move(value));
                    }
                }
            }
            sendRetries[producer] = retries;
        });
    }

    for (size_t consumer = 0; consumer < config.consumers; ++consumer) {
        consumers.emplace_back([&, consumer]() {
            placements[config.producers + consumer] =
                galay::benchmark::pinCurrentThread(config.producers + consumer);
            auto token = channel.makeConsumerToken();
            if (!token.valid()) {
                setupFailed.store(true, std::memory_order_release);
            }
            ready.arrive();
            start.wait();
            if (setupFailed.load(std::memory_order_acquire)) {
                return;
            }

            uint64_t localReceived = 0;
            uint64_t localChecksum = 0;
            uint64_t retries = 0;
            for (;;) {
                uint64_t rawValue = 0;
                std::optional<uint64_t> value;
                bool receivedValue = false;
                if constexpr (RawRecv) {
                    receivedValue =
                        galay::mpmc::UnboundedChannelTestAccess::rawRecv(channel, token, rawValue);
                } else {
                    value = channel.tryRecv(token);
                    receivedValue = value.has_value();
                }
                if (receivedValue) {
                    ++localReceived;
                    if constexpr (RawRecv) {
                        localChecksum += rawValue;
                    } else {
                        localChecksum += *value;
                    }
                    continue;
                }
                if (channel.isClosed()) {
                    break;
                }
                ++retries;
                std::this_thread::yield();
            }
            received[consumer] = localReceived;
            checksums[consumer] = localChecksum;
            emptyRetries[consumer] = retries;
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

    auto placement = galay::benchmark::ThreadPlacement::kPinnedToCore;
    for (const auto current : placements) {
        placement = std::max(placement, current);
    }
    return {
        .elapsedNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        .received = std::accumulate(received.begin(), received.end(), uint64_t{0}),
        .checksum = std::accumulate(checksums.begin(), checksums.end(), uint64_t{0}),
        .sendRetries = std::accumulate(sendRetries.begin(), sendRetries.end(), uint64_t{0}),
        .emptyRetries = std::accumulate(emptyRetries.begin(), emptyRetries.end(), uint64_t{0}),
        .finalSize = channel.size(),
        .placement = placement,
        .setupFailed = setupFailed.load(std::memory_order_acquire),
    };
}

Measurement runBoundedPath(const Config& config)
{
    galay::mpmc::BoundedChannel<uint64_t> channel(config.capacity);
    galay::benchmark::CompletionLatch ready(config.producers + config.consumers);
    galay::benchmark::StartGate start;
    std::vector<uint64_t> sendRetries(config.producers, 0);
    std::vector<uint64_t> emptyRetries(config.consumers, 0);
    std::vector<uint64_t> received(config.consumers, 0);
    std::vector<uint64_t> checksums(config.consumers, 0);
    std::vector<galay::benchmark::ThreadPlacement> placements(
        config.producers + config.consumers, galay::benchmark::ThreadPlacement::kUnsupported);
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(config.producers);
    consumers.reserve(config.consumers);

    for (size_t producer = 0; producer < config.producers; ++producer) {
        producers.emplace_back([&, producer]() {
            placements[producer] = galay::benchmark::pinCurrentThread(producer);
            ready.arrive();
            start.wait();
            const uint64_t first = config.messages * producer / config.producers;
            const uint64_t last = config.messages * (producer + 1) / config.producers;
            uint64_t retries = 0;
            for (uint64_t value = first; value < last; ++value) {
                while (!channel.trySend(std::move(value))) {
                    ++retries;
                    std::this_thread::yield();
                }
            }
            sendRetries[producer] = retries;
        });
    }

    for (size_t consumer = 0; consumer < config.consumers; ++consumer) {
        consumers.emplace_back([&, consumer]() {
            placements[config.producers + consumer] =
                galay::benchmark::pinCurrentThread(config.producers + consumer);
            ready.arrive();
            start.wait();
            uint64_t localReceived = 0;
            uint64_t localChecksum = 0;
            uint64_t retries = 0;
            for (;;) {
                auto value = channel.tryRecv();
                if (value.has_value()) {
                    ++localReceived;
                    localChecksum += *value;
                    continue;
                }
                if (channel.isClosed()) {
                    break;
                }
                ++retries;
                std::this_thread::yield();
            }
            received[consumer] = localReceived;
            checksums[consumer] = localChecksum;
            emptyRetries[consumer] = retries;
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

    auto placement = galay::benchmark::ThreadPlacement::kPinnedToCore;
    for (const auto current : placements) {
        placement = std::max(placement, current);
    }
    return {
        .elapsedNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        .received = std::accumulate(received.begin(), received.end(), uint64_t{0}),
        .checksum = std::accumulate(checksums.begin(), checksums.end(), uint64_t{0}),
        .sendRetries = std::accumulate(sendRetries.begin(), sendRetries.end(), uint64_t{0}),
        .emptyRetries = std::accumulate(emptyRetries.begin(), emptyRetries.end(), uint64_t{0}),
        .finalSize = channel.size(),
        .placement = placement,
    };
}

Measurement run(const Config& config)
{
    if (config.channelCase == ChannelCase::kBounded) {
        return runBoundedPath(config);
    }
    switch (config.path) {
    case Path::kToken:
        return runPath<false, false>(config);
    case Path::kRaw:
        return runPath<true, true>(config);
    case Path::kRawSend:
        return runPath<true, false>(config);
    case Path::kRawRecv:
        return runPath<false, true>(config);
    }
    return {.setupFailed = true};
}

bool placementValid(galay::benchmark::ThreadPlacement placement) noexcept
{
#if defined(__APPLE__)
    return placement == galay::benchmark::ThreadPlacement::kPerformanceClassOnly;
#elif defined(__linux__)
    return placement == galay::benchmark::ThreadPlacement::kPinnedToCore;
#else
    return true;
#endif
}

bool validMeasurement(const Config& config, const Measurement& result) noexcept
{
    return !result.setupFailed && result.elapsedNs > 0 && result.received == config.messages &&
           result.checksum == expectedChecksum(config.messages) && result.finalSize == 0 &&
           placementValid(result.placement);
}

} // namespace

int main(int argc, char** argv)
{
    auto config = parseArguments(argc, argv);
    if (!config) {
        std::cerr << "mpmc paired benchmark argument error: " << argumentErrorName(config.error())
                  << '\n';
        return 2;
    }

    const Measurement result = run(*config);
    const uint64_t expected = expectedChecksum(config->messages);
    const bool valid = validMeasurement(*config, result);
    const double messagesPerSecond = result.elapsedNs > 0
                                         ? static_cast<double>(config->messages) * 1'000'000'000.0 /
                                               static_cast<double>(result.elapsedNs)
                                         : 0.0;

    const char* const path =
        config->channelCase == ChannelCase::kBounded ? "direct" : pathName(config->path);
    const size_t capacity = config->channelCase == ChannelCase::kBounded ? config->capacity : 0;
    std::cout << "{\"schema\":\"galay.mpmc.paired.v2\""
              << ",\"language\":\"cpp\""
              << ",\"case\":\"" << caseName(config->channelCase) << "\""
              << ",\"path\":\"" << path << "\""
              << ",\"topology\":\"" << config->producers << 'p' << config->consumers << "c\""
              << ",\"payload_bytes\":8"
              << ",\"capacity\":" << capacity << ",\"messages\":" << config->messages
              << ",\"elapsed_ns\":" << result.elapsedNs
              << ",\"messages_per_second\":" << messagesPerSecond
              << ",\"received\":" << result.received << ",\"checksum\":" << result.checksum
              << ",\"expected_checksum\":" << expected << ",\"send_retries\":" << result.sendRetries
              << ",\"empty_retries\":" << result.emptyRetries << ",\"placement\":\""
              << galay::benchmark::threadPlacementName(result.placement) << "\""
              << ",\"backoff\":\"yield\""
              << ",\"generator\":\"partitioned_monotonic_u64\""
              << ",\"valid\":" << (valid ? "true" : "false") << "}\n";
    return valid ? 0 : 1;
}
