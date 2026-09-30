/**
 * @file b42_ready_ring_lifecycle.cc
 * @brief Ready ring push/pop/release cost, with fixed work and separate warmup.
 * @note The shared samples exercise the stealing-enabled owner path without
 *       concurrent stealers; concurrency correctness belongs to T100.
 */

#include "../common/benchmark_environment.h"
#include "../common/micro_measurement.h"

#include <galay/cpp/galay-kernel/core/io_ready_queue.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string_view>

using namespace galay::kernel;

namespace {

struct EntryProbe {
    detail::ReadyEntryCoroHeader header;
    size_t releases = 0;
};

void releaseProbe(void* state) noexcept {
    ++static_cast<EntryProbe*>(state)->releases;
}

constexpr detail::ReadyEntryHooks kHooks{
    .owner_scheduler = nullptr,
    .resume_owner_only = nullptr,
    .resume = nullptr,
    .release = releaseProbe,
};

bool measure(std::string_view sample, size_t iterations, bool print) {
    const bool shared = sample == "shared-1" || sample == "shared-64";
    const size_t batch = sample == "owner-64" || sample == "shared-64" ? 64 : 1;
    std::array<EntryProbe, 64> probes{};
    for (auto& probe : probes) { probe.header.hooks = &kHooks; }
    ChaseLevTaskRing ring;
    ring.setStealingEnabled(shared);
    size_t completed = 0;
    size_t errors = 0;
    galay::benchmark::MicroMeasurement measurement;
    galay::benchmark::MicroTimer timer;
    while (completed < iterations) {
        const size_t count = std::min(batch, iterations - completed);
        for (size_t i = 0; i < count; ++i) {
            detail::ReadyEntry entry(detail::ReadyEntryKind::CCoroutine, &probes[i]);
            if (!ring.push_back(entry)) {
                detail::releaseReadyEntry(entry);
                ++errors;
                break;
            }
        }
        if (errors != 0) { break; }
        // Keep the real ring state observable across fill/drain without a CPU fence.
        asm volatile("" : : "g"(&ring) : "memory");
        for (size_t i = count; i > 0; --i) {
            detail::ReadyEntry entry;
            if (!ring.pop_back(entry)) { ++errors; break; }
            errors += entry.state() != &probes[i - 1];
            detail::releaseReadyEntry(entry);
            ++completed;
        }
        if (errors != 0) { break; }
    }
    timer.finish(measurement);
    size_t released = 0;
    for (const auto& probe : probes) { released += probe.releases; }
    const bool valid = measurement.cpu_ns >= 0 && errors == 0 && completed == iterations &&
        released == iterations && ring.empty();
    if (print) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            measurement.elapsed).count();
        std::cout << "ReadyRing sample=" << sample << " iterations=" << completed
                  << " elapsed_ns=" << elapsed << " cpu_ns=" << measurement.cpu_ns
                  << " ns_per_op=" << std::fixed << std::setprecision(3)
                  << (completed == 0 ? 0.0 : static_cast<double>(elapsed) / completed)
                  << " releases=" << released << " errors=" << errors << '\n';
    }
    return valid;
}

} // namespace

int main(int argc, char** argv) {
    if (!galay::benchmark::initializeBenchmarkEnvironment()) { return 1; }
    galay::benchmark::MicroOptions options;
    if (!galay::benchmark::parseMicroOptions(argc, argv, options)) {
        std::cerr << "Usage: --sample owner-1|owner-64|shared-1|shared-64 --iterations N\n";
        return 1;
    }
    constexpr std::array<std::string_view, 4> samples{
        "owner-1", "owner-64", "shared-1", "shared-64"};
    bool matched = false;
    for (const auto sample : samples) {
        if (!options.sample.empty() && options.sample != sample) { continue; }
        matched = true;
        if (!measure(sample, 20'000, false) ||
            !measure(sample, options.iterations, true)) {
            std::cerr << "B42-ReadyRingLifecycle FAIL\n";
            return 1;
        }
    }
    if (!matched) {
        std::cerr << "Unknown sample: " << options.sample << '\n';
        return 1;
    }
    return 0;
}
