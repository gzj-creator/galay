/**
 * @file b42_ready_ring_lifecycle.cc
 * @brief Ready ring push/pop/release cost, with fixed work and separate warmup.
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

void release_probe(void* state) noexcept {
    ++static_cast<EntryProbe*>(state)->releases;
}

constexpr detail::ReadyEntryHooks kHooks{
    .owner_scheduler = nullptr,
    .resume = nullptr,
    .release = release_probe,
};

bool measure(std::string_view sample, size_t iterations, bool print) {
    const size_t batch = sample == "owner-64" ? 64 : 1;
    std::array<EntryProbe, 64> probes{};
    for (auto& probe : probes) { probe.header.hooks = &kHooks; }
    LocalReadyStack stack;
    size_t completed = 0;
    size_t errors = 0;
    galay::benchmark::MicroMeasurement measurement;
    galay::benchmark::MicroTimer timer;
    while (completed < iterations) {
        const size_t count = std::min(batch, iterations - completed);
        for (size_t i = 0; i < count; ++i) {
            detail::ReadyEntry entry(detail::ReadyEntryKind::CCoroutine, &probes[i]);
            if (!stack.push_back(entry)) {
                detail::release_ready_entry(entry);
                ++errors;
                break;
            }
        }
        if (errors != 0) { break; }
        // Keep the real stack state observable across fill/drain without a CPU fence.
        asm volatile("" : : "g"(&stack) : "memory");
        for (size_t i = count; i > 0; --i) {
            detail::ReadyEntry entry;
            if (!stack.pop_back(entry)) { ++errors; break; }
            errors += entry.state() != &probes[i - 1];
            detail::release_ready_entry(entry);
            ++completed;
        }
        if (errors != 0) { break; }
    }
    timer.finish(measurement);
    size_t released = 0;
    for (const auto& probe : probes) { released += probe.releases; }
    const bool valid = measurement.cpu_ns >= 0 && errors == 0 && completed == iterations &&
        released == iterations && stack.empty();
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
    if (!galay::benchmark::initialize_benchmark_environment()) { return 1; }
    galay::benchmark::MicroOptions options;
    if (!galay::benchmark::parse_micro_options(argc, argv, options)) {
        std::cerr << "Usage: --sample owner-1|owner-64 --iterations N\n";
        return 1;
    }
    constexpr std::array<std::string_view, 2> samples{"owner-1", "owner-64"};
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
