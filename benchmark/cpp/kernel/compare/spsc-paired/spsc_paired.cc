// Historical/internal-only fixture. Not a formal competitor baseline; see docs/cpp/modules/kernel/05-性能测试.md.
/**
 * @file spsc_paired.cc
 * @brief 测量 SPSC ring/channel 的 scalar/batch bounded 及 unbounded 吞吐。
 */

#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>
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
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace {

enum class CaseKind : uint8_t {
    kRawBounded,
    kChannelBounded,
    kBatchBounded,
    kUnbounded,
    kBatchUnbounded,
};

enum class BackoffKind : uint8_t {
    kYield,
    kSpin,
    kHybrid,
};

enum class ArgumentError : uint8_t {
    kMissingValue,
    kUnknownOption,
    kInvalidCase,
    kInvalidNumber,
    kInvalidCapacity,
    kInvalidBatchSize,
    kInvalidBackoff,
    kInvalidCorePair,
};

struct Config {
    uint64_t messages = 1'000'000;
    size_t capacity = 4096;
    size_t batchSize = 64;
    size_t producerCore = 0;
    size_t consumerCore = 1;
    CaseKind kind = CaseKind::kRawBounded;
    BackoffKind backoff = BackoffKind::kYield;
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

struct alignas(galay::utils::kCacheLineSize) StartState {
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
};

struct alignas(galay::utils::kCacheLineSize) ProducerResult {
    uint64_t fullRetries = 0;
    galay::benchmark::ThreadPlacement placement =
        galay::benchmark::ThreadPlacement::kUnsupported;
    bool readyOk = true;
    bool sendOk = true;
};

struct alignas(galay::utils::kCacheLineSize) ConsumerResult {
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
    case ArgumentError::kInvalidBatchSize:
        return "batch size must be positive";
    case ArgumentError::kInvalidBackoff:
        return "backoff must be yield, spin, or hybrid";
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
    case CaseKind::kChannelBounded:
        return "channel_bounded";
    case CaseKind::kBatchBounded:
        return "batch_bounded";
    case CaseKind::kUnbounded:
        return "unbounded";
    case CaseKind::kBatchUnbounded:
        return "batch_unbounded";
    }
    return "unknown";
}

const char* implementationName(CaseKind kind) noexcept
{
    switch (kind) {
    case CaseKind::kRawBounded:
        return "galay::spsc::Ring::split";
    case CaseKind::kChannelBounded:
        return "galay::spsc::BoundedChannel";
    case CaseKind::kBatchBounded:
        return "galay::spsc::Ring::split batch";
    case CaseKind::kUnbounded:
        return "galay::spsc::UnboundedChannel";
    case CaseKind::kBatchUnbounded:
        return "galay::spsc::UnboundedChannel::sendBatch/tryRecvBatch";
    }
    return "unknown";
}

const char* apiProfile(CaseKind kind) noexcept
{
    switch (kind) {
    case CaseKind::kRawBounded:
        return "bounded_spsc_polling_split";
    case CaseKind::kChannelBounded:
        return "bounded_spsc_wait_capable_polling_path";
    case CaseKind::kBatchBounded:
        return "bounded_spsc_batch_polling_split";
    case CaseKind::kUnbounded:
        return "unbounded_spsc_wait_capable_polling_path";
    case CaseKind::kBatchUnbounded:
        return "unbounded_spsc_batch_polling";
    }
    return "unknown";
}

const char* comparisonScope(CaseKind kind) noexcept
{
    switch (kind) {
    case CaseKind::kRawBounded:
    case CaseKind::kBatchBounded:
        return "equivalent_measured_api";
    case CaseKind::kChannelBounded:
        return "internal_regression_guard";
    case CaseKind::kUnbounded:
        return "nearest_available_measured_path";
    case CaseKind::kBatchUnbounded:
        return "reference_only_no_equivalent_rust_batch_api";
    }
    return "unknown";
}

const char* backoffName(BackoffKind kind) noexcept
{
    switch (kind) {
    case BackoffKind::kYield:
        return "yield";
    case BackoffKind::kSpin:
        return "spin";
    case BackoffKind::kHybrid:
        return "hybrid";
    }
    return "unknown";
}

void cpuPause() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__APPLE__) && defined(__aarch64__)
    __asm__ __volatile__("isb" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

class Backoff
{
public:
    explicit Backoff(BackoffKind kind) noexcept : m_kind(kind) {}

    void reset() noexcept
    {
        m_failures = 0;
    }

    void wait() noexcept
    {
        constexpr size_t kRetryLimit = 64;
        constexpr size_t kMaximumSpinExponent = 6;
        switch (m_kind) {
        case BackoffKind::kYield:
            std::this_thread::yield();
            break;
        case BackoffKind::kSpin:
            if (m_failures < kRetryLimit) {
                const size_t pauses = size_t{1} <<
                    std::min(m_failures, kMaximumSpinExponent);
                for (size_t pause = 0; pause < pauses; ++pause) {
                    cpuPause();
                }
            } else {
                std::this_thread::yield();
            }
            break;
        case BackoffKind::kHybrid:
            if (m_failures < kRetryLimit) {
                cpuPause();
            } else {
                std::this_thread::yield();
            }
            break;
        }
        if (m_failures != std::numeric_limits<size_t>::max()) {
            ++m_failures;
        }
    }

private:
    size_t m_failures = 0;
    BackoffKind m_kind;
};

bool isBounded(CaseKind kind) noexcept
{
    return kind == CaseKind::kRawBounded ||
        kind == CaseKind::kChannelBounded ||
        kind == CaseKind::kBatchBounded;
}

bool isBatch(CaseKind kind) noexcept
{
    return kind == CaseKind::kBatchBounded || kind == CaseKind::kBatchUnbounded;
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
            } else if (value == "channel_bounded") {
                config.kind = CaseKind::kChannelBounded;
            } else if (value == "batch_bounded") {
                config.kind = CaseKind::kBatchBounded;
            } else if (value == "unbounded") {
                config.kind = CaseKind::kUnbounded;
            } else if (value == "batch_unbounded") {
                config.kind = CaseKind::kBatchUnbounded;
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
        } else if (option == "--batch-size") {
            auto parsed = parseUnsigned<size_t>(value);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            config.batchSize = *parsed;
        } else if (option == "--backoff") {
            if (value == "yield") {
                config.backoff = BackoffKind::kYield;
            } else if (value == "spin") {
                config.backoff = BackoffKind::kSpin;
            } else if (value == "hybrid") {
                config.backoff = BackoffKind::kHybrid;
            } else {
                return std::unexpected(ArgumentError::kInvalidBackoff);
            }
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

    if (isBounded(config.kind) &&
        (config.capacity < 2 || (config.capacity & (config.capacity - 1)) != 0)) {
        return std::unexpected(ArgumentError::kInvalidCapacity);
    }
    if (isBatch(config.kind) && config.batchSize == 0) {
        return std::unexpected(ArgumentError::kInvalidBatchSize);
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
    ProducerResult producerOutput;
    ConsumerResult consumerOutput;

    std::thread producer([
        &, send = std::forward<Send>(send)]() mutable {
        ProducerResult producerResult;
        producerResult.placement =
            galay::benchmark::pinCurrentThread(config.producerCore);
        const size_t readyCount =
            state.ready.fetch_add(1, std::memory_order_release) + 1;
        producerResult.readyOk = readyCount <= 2;
        while (!state.start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        Backoff backoff(config.backoff);
        for (uint64_t sequence = 0; sequence < config.messages; ++sequence) {
            uint64_t pending = sequence;
            while (!send(pending)) {
                ++producerResult.fullRetries;
                backoff.wait();
            }
            backoff.reset();
        }
        producerOutput = producerResult;
    });

    std::thread consumer([
        &, receive = std::forward<Receive>(receive)]() mutable {
        ConsumerResult consumerResult;
        consumerResult.placement =
            galay::benchmark::pinCurrentThread(config.consumerCore);
        const size_t readyCount =
            state.ready.fetch_add(1, std::memory_order_release) + 1;
        consumerResult.readyOk = readyCount <= 2;
        while (!state.start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        Backoff backoff(config.backoff);
        while (consumerResult.received < config.messages) {
            uint64_t value = 0;
            if (receive(value)) {
                consumerResult.fifoOk =
                    consumerResult.fifoOk && value == consumerResult.received;
                consumerResult.checksum += value;
                ++consumerResult.received;
                backoff.reset();
                continue;
            }
            ++consumerResult.emptyRetries;
            // 只按预期数量结束，避免 producer 完成后的瞬时空读造成伪失败。
            backoff.wait();
        }
        consumerOutput = consumerResult;
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
    const bool sendOk = producerOutput.readyOk && producerOutput.sendOk &&
        (producerOutput.fullRetries == 0 || isBounded(config.kind));

    return {
        .messagesPerSecond = elapsedValue > 0
            ? static_cast<double>(config.messages) * 1'000'000'000.0 /
                static_cast<double>(elapsedValue)
            : 0.0,
        .elapsedNs = elapsedValue,
        .received = consumerOutput.received,
        .checksum = consumerOutput.checksum,
        .expectedChecksum = expected,
        .fullRetries = producerOutput.fullRetries,
        .emptyRetries = consumerOutput.emptyRetries,
        .producerPlacement = producerOutput.placement,
        .consumerPlacement = consumerOutput.placement,
        .fifoOk = consumerOutput.readyOk && consumerOutput.fifoOk,
        .sendOk = sendOk,
    };
}

Measurement runBounded(const Config& config)
{
    galay::spsc::Ring<uint64_t> channel(config.capacity);
    if (channel.error() != galay::spsc::RingError::kNone) {
        return {};
    }
    auto endpoints = channel.split();
    return runPair(
        config,
        [producer = std::move(endpoints.producer)](uint64_t& value) mutable {
            return producer.tryWrite(std::move(value));
        },
        [consumer = std::move(endpoints.consumer)](uint64_t& value) mutable {
            return consumer.tryRead(value);
        });
}

Measurement runChannelBounded(const Config& config)
{
    galay::spsc::BoundedChannel<uint64_t> channel(config.capacity);
    if (channel.error() != galay::spsc::RingError::kNone) {
        return {};
    }
    return runPair(
        config,
        [&channel](uint64_t& value) {
            return channel.trySend(std::move(value));
        },
        [&channel](uint64_t& value) {
            auto received = channel.tryRecv();
            if (!received.has_value()) {
                return false;
            }
            value = std::move(*received);
            return true;
        });
}

Measurement runUnbounded(const Config& config)
{
    galay::spsc::UnboundedChannel<uint64_t> channel;
    if (!channel.valid()) {
        return {};
    }
    return runPair(
        config,
        [&channel](uint64_t& value) { return channel.send(std::move(value)); },
        [&channel](uint64_t& value) {
            auto received = channel.tryRecv();
            if (!received.has_value()) {
                return false;
            }
            value = std::move(*received);
            return true;
        });
}

template <typename SendBatch, typename RecvBatch>
Measurement runBatchPair(const Config& config,
                         SendBatch&& sendBatch,
                         RecvBatch&& recvBatch)
{
    StartState state;
    ProducerResult producerOutput;
    ConsumerResult consumerOutput;
    std::vector<uint64_t> producerValues(config.batchSize);
    std::vector<uint64_t> consumerValues(config.batchSize);

    std::thread producer([
        &, values = std::move(producerValues),
        send = std::forward<SendBatch>(sendBatch)]() mutable {
        ProducerResult producerResult;
        producerResult.placement =
            galay::benchmark::pinCurrentThread(config.producerCore);
        const size_t readyCount =
            state.ready.fetch_add(1, std::memory_order_release) + 1;
        producerResult.readyOk = readyCount <= 2;
        while (!state.start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        Backoff backoff(config.backoff);
        uint64_t sent = 0;
        while (sent < config.messages) {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(
                config.batchSize, config.messages - sent));
            for (size_t offset = 0; offset < count; ++offset) {
                values[offset] = sent + offset;
            }
            size_t offset = 0;
            while (offset < count) {
                auto published = send(values, offset, count - offset);
                if (!published.has_value()) {
                    producerResult.sendOk = false;
                    state.failed.store(true, std::memory_order_release);
                    producerOutput = producerResult;
                    return;
                }
                if (*published == 0) {
                    ++producerResult.fullRetries;
                    backoff.wait();
                    continue;
                }
                offset += *published;
                sent += *published;
                backoff.reset();
            }
        }
        producerOutput = producerResult;
    });

    std::thread consumer([
        &, values = std::move(consumerValues),
        receive = std::forward<RecvBatch>(recvBatch)]() mutable {
        ConsumerResult consumerResult;
        consumerResult.placement =
            galay::benchmark::pinCurrentThread(config.consumerCore);
        const size_t readyCount =
            state.ready.fetch_add(1, std::memory_order_release) + 1;
        consumerResult.readyOk = readyCount <= 2;
        while (!state.start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        Backoff backoff(config.backoff);
        while (consumerResult.received < config.messages) {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(
                config.batchSize, config.messages - consumerResult.received));
            const size_t received = receive(std::span<uint64_t>(values).first(count));
            if (received == 0) {
                if (state.failed.load(std::memory_order_acquire)) {
                    consumerResult.fifoOk = false;
                    break;
                }
                ++consumerResult.emptyRetries;
                backoff.wait();
                continue;
            }
            const uint64_t firstExpected = consumerResult.received;
            uint64_t batchChecksum = 0;
            for (size_t offset = 0; offset < received; ++offset) {
                batchChecksum += values[offset];
            }
            uint64_t orderMismatch = 0;
            for (size_t offset = 0; offset < received; ++offset) {
                orderMismatch |=
                    values[offset] ^
                    (firstExpected + static_cast<uint64_t>(offset));
            }
            consumerResult.fifoOk =
                consumerResult.fifoOk && orderMismatch == 0;
            consumerResult.checksum += batchChecksum;
            consumerResult.received += received;
            backoff.reset();
        }
        consumerOutput = consumerResult;
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

    return {
        .messagesPerSecond = elapsedValue > 0
            ? static_cast<double>(config.messages) * 1'000'000'000.0 /
                static_cast<double>(elapsedValue)
            : 0.0,
        .elapsedNs = elapsedValue,
        .received = consumerOutput.received,
        .checksum = consumerOutput.checksum,
        .expectedChecksum = expectedChecksum(config.messages),
        .fullRetries = producerOutput.fullRetries,
        .emptyRetries = consumerOutput.emptyRetries,
        .producerPlacement = producerOutput.placement,
        .consumerPlacement = consumerOutput.placement,
        .fifoOk = consumerOutput.readyOk && consumerOutput.fifoOk,
        .sendOk = producerOutput.readyOk && producerOutput.sendOk,
    };
}

Measurement runBatchBounded(const Config& config)
{
    galay::spsc::Ring<uint64_t> channel(config.capacity);
    if (channel.error() != galay::spsc::RingError::kNone) {
        return {};
    }
    auto endpoints = channel.split();
    return runBatchPair(
        config,
        [producer = std::move(endpoints.producer)](
            std::vector<uint64_t>& values,
            size_t offset,
            size_t count) mutable -> std::optional<size_t> {
            return producer.tryWriteBatch(
                std::span<uint64_t>(values).subspan(offset, count));
        },
        [consumer = std::move(endpoints.consumer)](
            std::span<uint64_t> values) mutable {
            return consumer.tryReadBatch(values);
        });
}

Measurement runBatchUnbounded(const Config& config)
{
    galay::spsc::UnboundedChannel<uint64_t> channel;
    if (!channel.valid()) {
        return {};
    }
    return runBatchPair(
        config,
        [&channel](std::vector<uint64_t>& values,
                   size_t offset,
                   size_t count) -> std::optional<size_t> {
            if (offset != 0) {
                return std::nullopt;
            }
            if (values.size() != count) {
                values.resize(count);
            }
            if (!channel.sendBatch(std::move(values))) {
                return std::nullopt;
            }
            return count;
        },
        [&channel](std::span<uint64_t> values) {
            return channel.tryRecvBatch(values);
        });
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

    const Measurement measurement = [&]() {
        switch (config->kind) {
        case CaseKind::kRawBounded:
            return runBounded(*config);
        case CaseKind::kChannelBounded:
            return runChannelBounded(*config);
        case CaseKind::kBatchBounded:
            return runBatchBounded(*config);
        case CaseKind::kUnbounded:
            return runUnbounded(*config);
        case CaseKind::kBatchUnbounded:
            return runBatchUnbounded(*config);
        }
        return Measurement{};
    }();
    const bool valid = measurement.elapsedNs > 0 && measurement.sendOk &&
        measurement.received == config->messages && measurement.fifoOk &&
        measurement.checksum == measurement.expectedChecksum;
    const size_t reportedCapacity =
        isBounded(config->kind) ? config->capacity : 0;
    const size_t reportedBatchSize =
        isBatch(config->kind) ? config->batchSize : 1;

    std::cout << std::setprecision(17) << std::boolalpha
              << "{\"schema\":\"galay.spsc.paired.v4\""
              << ",\"language\":\"cpp\""
              << ",\"case\":\"" << caseName(config->kind) << "\""
              << ",\"implementation\":\"" << implementationName(config->kind) << "\""
              << ",\"api_profile\":\"" << apiProfile(config->kind) << "\""
              << ",\"comparison_scope\":\"" << comparisonScope(config->kind) << "\""
              << ",\"topology\":\"1p1c\""
              << ",\"payload_bytes\":8"
              << ",\"capacity\":" << reportedCapacity
              << ",\"batch_size\":" << reportedBatchSize
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
              << ",\"backoff\":\"" << backoffName(config->backoff) << "\""
              << ",\"generator\":\"monotonic_u64\""
              << ",\"valid\":" << valid << "}\n";
    if (!std::cout.good()) {
        return 3;
    }
    return valid ? 0 : 1;
}
