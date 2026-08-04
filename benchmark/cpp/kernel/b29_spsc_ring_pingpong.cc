/**
 * @file b29_spsc_ring_pingpong.cc
 * @brief 隔离测量 SPSC ring 单线程 send/receive 往返成本。
 */

#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <utility>

namespace {

constexpr size_t kCapacity = 4096;
constexpr uint64_t kWarmupIterations = 1'000'000;
constexpr uint64_t kMeasuredIterations = 50'000'000;
constexpr size_t kRounds = 3;
constexpr double kBaselineUpperNs = 0.89;
constexpr double kRegressionThresholdNs = kBaselineUpperNs * 1.30;

struct RoundResult {
    uint64_t checksum = 0;
    bool valid = true;
};

template <typename Producer, typename Consumer>
[[nodiscard]] RoundResult runRound(Producer& producer,
                                   Consumer& consumer,
                                   uint64_t iterations) noexcept
{
    RoundResult result;
    for (uint64_t sequence = 0; sequence < iterations; ++sequence) {
        uint64_t pending = sequence;
        if (!producer.trySend(std::move(pending))) {
            result.valid = false;
            break;
        }
        uint64_t value = 0;
        if (!consumer.tryRecv(value)) {
            result.valid = false;
            break;
        }
        result.checksum += value;
    }
    return result;
}

[[nodiscard]] uint64_t expectedChecksum(uint64_t iterations) noexcept
{
    return (iterations & 1U) == 0
        ? (iterations / 2) * (iterations - 1)
        : iterations * ((iterations - 1) / 2);
}

} // namespace

int main()
{
    galay::spsc::Ring<uint64_t> ring(kCapacity);
    if (ring.error() != galay::spsc::RingError::kNone) {
        std::cerr << "failed to construct SPSC ring: "
                  << galay::spsc::ringErrorString(ring.error()) << '\n';
        return std::cerr.good() ? 2 : 3;
    }
    auto endpoints = ring.split();

    const RoundResult warmup = runRound(
        endpoints.producer, endpoints.consumer, kWarmupIterations);
    if (!warmup.valid || warmup.checksum != expectedChecksum(kWarmupIterations)) {
        std::cerr << "SPSC ring ping-pong warmup validation failed\n";
        return std::cerr.good() ? 1 : 3;
    }

    std::array<double, kRounds> roundTripNs{};
    std::array<double, kRounds> roundTripsPerSecond{};
    bool valid = true;
    for (size_t round = 0; round < kRounds; ++round) {
        const auto begin = std::chrono::steady_clock::now();
        const RoundResult result = runRound(
            endpoints.producer, endpoints.consumer, kMeasuredIterations);
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        const auto elapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        valid = valid && result.valid && elapsedNs > 0 &&
            result.checksum == expectedChecksum(kMeasuredIterations);
        if (elapsedNs <= 0) {
            continue;
        }
        roundTripNs[round] = static_cast<double>(elapsedNs) /
            static_cast<double>(kMeasuredIterations);
        roundTripsPerSecond[round] = static_cast<double>(kMeasuredIterations) *
            1'000'000'000.0 / static_cast<double>(elapsedNs);
    }

    auto sortedRoundTripNs = roundTripNs;
    auto sortedRoundTripsPerSecond = roundTripsPerSecond;
    std::sort(sortedRoundTripNs.begin(), sortedRoundTripNs.end());
    std::sort(sortedRoundTripsPerSecond.begin(), sortedRoundTripsPerSecond.end());
    const double medianRoundTripNs = sortedRoundTripNs[kRounds / 2];
    const double medianRoundTripsPerSecond =
        sortedRoundTripsPerSecond[kRounds / 2];
    const bool baselineRegression = medianRoundTripNs > kRegressionThresholdNs;

    std::cout << std::setprecision(17) << std::boolalpha
              << "{\"schema\":\"galay.spsc.ring_pingpong.v1\""
              << ",\"capacity\":" << kCapacity
              << ",\"iterations\":" << kMeasuredIterations
              << ",\"round_trip_ns\":[";
    for (size_t round = 0; round < kRounds; ++round) {
        if (round != 0) {
            std::cout << ',';
        }
        std::cout << roundTripNs[round];
    }
    std::cout << "]"
              << ",\"round_trips_per_second\":[";
    for (size_t round = 0; round < kRounds; ++round) {
        if (round != 0) {
            std::cout << ',';
        }
        std::cout << roundTripsPerSecond[round];
    }
    std::cout << "]"
              << ",\"median_round_trip_ns\":" << medianRoundTripNs
              << ",\"median_round_trips_per_second\":"
              << medianRoundTripsPerSecond
              << ",\"baseline_upper_ns\":" << kBaselineUpperNs
              << ",\"regression_threshold_ns\":" << kRegressionThresholdNs
              << ",\"baseline_regression\":" << baselineRegression
              << ",\"valid\":" << valid << "}\n";
    if (!std::cout.good()) {
        return 3;
    }
    return valid ? 0 : 1;
}
