#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-utils/cache/lru_cache.hpp>
#include <galay/cpp/galay-utils/system/rdtsc.hpp>
#include <galay/cpp/galay-utils/system/time.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <system_error>
#include <type_traits>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace {

volatile std::uint64_t g_sink = 0;
constexpr std::size_t kRounds = 9;
constexpr std::size_t kScenarios = 12;

struct Result {
    const char* name = "";
    double ns_per_op = 0;
    std::uint64_t checksum = 0;
};

template<typename Fn>
std::expected<Result, std::error_code> measure(const char* name, std::size_t iterations, Fn&& fn) {
    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
        const auto value = fn(index);
        if constexpr (std::is_integral_v<decltype(value)>) {
            checksum += value;
        } else {
            if (!value) {
                return std::unexpected(value.error());
            }
            checksum += *value;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    g_sink = checksum;
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return Result{name, static_cast<double>(elapsed_ns) / static_cast<double>(iterations), checksum};
}

std::expected<Result, std::error_code> run_scenario(
    std::size_t scenario, std::size_t iterations, const galay::utils::Rdtsc& counter) {
    const auto origin = counter.read_start();
    switch (scenario) {
    case 0:
        return measure("steady_clock::now", iterations, [](std::size_t) {
            return static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
        });
    case 1:
        return measure("system_clock::now", iterations, [](std::size_t) {
            return static_cast<std::uint64_t>(galay::utils::Time::current_time_ns());
        });
    case 2:
        return measure("rdtsc relaxed", iterations, [&](std::size_t) {
            return counter.read_relaxed();
        });
    case 3:
        return measure("rdtsc start", iterations, [&](std::size_t) {
            return counter.read_start();
        });
    case 4:
        return measure("rdtsc end", iterations, [&](std::size_t) {
            return counter.read_end();
        });
    case 5:
        return measure("relaxed + ns Q32", iterations, [&](std::size_t)
            -> std::expected<std::uint64_t, std::error_code> {
            const auto end = counter.read_relaxed();
            if (end < origin) {
                return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
            }
            const auto ns = counter.to_nanoseconds(end - origin);
            if (!ns) {
                return std::unexpected(ns.error());
            }
            return static_cast<std::uint64_t>(ns->count());
        });
    case 6:
        return measure("ordered + ns Q32", iterations, [&](std::size_t)
            -> std::expected<std::uint64_t, std::error_code> {
            const auto end = counter.read_end();
            if (end < origin) {
                return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
            }
            const auto ns = counter.to_nanoseconds(end - origin);
            if (!ns) {
                return std::unexpected(ns.error());
            }
            return static_cast<std::uint64_t>(ns->count());
        });
    case 7:
        return measure("steady interval", iterations, [](std::size_t) {
            const auto start = std::chrono::steady_clock::now();
            const auto end = std::chrono::steady_clock::now();
            return static_cast<std::uint64_t>((end - start).count());
        });
    case 8:
        return measure("ordered interval + ns", iterations, [&](std::size_t)
            -> std::expected<std::uint64_t, std::error_code> {
            const auto start = counter.read_start();
            const auto end = counter.read_end();
            if (end < start) {
                return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
            }
            const auto ns = counter.to_nanoseconds(end - start);
            if (!ns) {
                return std::unexpected(ns.error());
            }
            return static_cast<std::uint64_t>(ns->count());
        });
    case 9: {
        const double ns_per_tick = 1'000'000'000.0 / static_cast<double>(counter.frequency_hz());
        return measure("relaxed + ns double", iterations, [&](std::size_t)
            -> std::expected<std::uint64_t, std::error_code> {
            const auto end = counter.read_relaxed();
            if (end < origin) {
                return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
            }
            return static_cast<std::uint64_t>(
                static_cast<double>(end - origin) * ns_per_tick);
        });
    }
    case 10:
#if defined(__x86_64__)
        return measure("intrinsic __rdtsc", iterations, [](std::size_t) {
            return static_cast<std::uint64_t>(__rdtsc());
        });
#else
        return measure("relaxed counter control", iterations, [&](std::size_t) {
            return counter.read_relaxed();
        });
#endif
    default:
        return measure("loop control", iterations, [](std::size_t index) {
            // 编译器屏障防止对照循环被折叠，保留真实的逐次迭代开销。
#if defined(__GNUC__) || defined(__clang__)
            __asm__ volatile("" : "+r"(index));
#endif
            return static_cast<std::uint64_t>(index);
        });
    }
}

std::expected<Result, std::error_code> run_cache(
    std::size_t scenario, std::size_t iterations, const galay::utils::Rdtsc& counter) {
    galay::utils::LruCache<int, int> cache(64);
    for (int key = 0; key < 64; ++key) {
        if (!cache.put(key, key + 1)) {
            return std::unexpected(std::make_error_code(std::errc::state_not_recoverable));
        }
    }
    const auto get_value = [&](std::size_t index) {
        const auto* value = cache.get(static_cast<int>(index & 63U));
        return value ? static_cast<std::uint64_t>(*value) : 0;
    };
    if (scenario == 0) {
        return measure("LRU no per-op timing", iterations, get_value);
    }
    if (scenario == 1) {
        return measure("LRU steady per-op timing", iterations, [&](std::size_t index) {
            const auto start = std::chrono::steady_clock::now();
            const auto value = get_value(index);
            const auto end = std::chrono::steady_clock::now();
            return value + static_cast<std::uint64_t>((end - start).count());
        });
    }
    return measure("LRU rdtsc per-op timing", iterations, [&](std::size_t index)
        -> std::expected<std::uint64_t, std::error_code> {
        const auto start = counter.read_start();
        const auto value = get_value(index);
        const auto end = counter.read_end();
        if (end < start) {
            return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
        }
        const auto ns = counter.to_nanoseconds(end - start);
        if (!ns) {
            return std::unexpected(ns.error());
        }
        return value + static_cast<std::uint64_t>(ns->count());
    });
}

template<std::size_t Size>
void print_summary(const std::array<std::array<Result, kRounds>, Size>& results) {
    std::cout << "SUMMARY scenario,median_ns_per_op,min_ns_per_op,max_ns_per_op\n";
    for (const auto& samples : results) {
        std::array<double, kRounds> sorted{};
        for (std::size_t round = 0; round < kRounds; ++round) {
            sorted[round] = samples[round].ns_per_op;
        }
        std::sort(sorted.begin(), sorted.end());
        std::cout << samples.front().name << ',' << sorted[kRounds / 2] << ','
                  << sorted.front() << ',' << sorted.back() << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }
#if defined(__linux__)
    const auto affinity = galay::utils::CPU::cpu_affinity();
    if (!affinity || affinity->size() != 1) {
        std::cerr << "time_sources requires one CPU in GALAY_BENCH_CPUS\n";
        return 1;
    }
#else
    std::cerr << "time_sources requires an externally pinned measuring thread\n";
    return 1;
#endif
    std::size_t iterations = 2'000'000;
    if (argc == 2) {
        const std::string_view text{argv[1]};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), iterations);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || iterations == 0) {
            std::cerr << "invalid iteration count\n";
            return 1;
        }
    } else if (argc != 1) {
        std::cerr << "usage: benchmark_utils_time_sources [iterations]\n";
        return 1;
    }
    const auto counter = galay::utils::Rdtsc::create();
    if (!counter) {
        std::cerr << "counter initialization failed: " << counter.error().message() << '\n';
        return 1;
    }
    const auto origin = counter->read_start();
    const auto origin_reference = std::chrono::steady_clock::now();
    std::cout << "frequency_hz=" << counter->frequency_hz()
              << " invariant=" << counter->has_invariant_frequency()
              << " source=" << (counter->frequency_source() == galay::utils::RdtscFrequencySource::kCalibrated
                                    ? "calibrated" : "architectural")
              << " iterations=" << iterations << " rounds=" << kRounds << '\n';
    std::cout << std::fixed << std::setprecision(3);
    for (std::size_t scenario = 0; scenario < kScenarios; ++scenario) {
        const auto warmup = run_scenario(scenario, 100'000, *counter);
        if (!warmup) {
            std::cerr << "warmup failed: " << warmup.error().message() << '\n';
            return 1;
        }
    }
    std::array<std::array<Result, kRounds>, kScenarios> results{};
    std::cout << "SAMPLES round,scenario,ns_per_op,checksum\n";
    for (std::size_t round = 0; round < kRounds; ++round) {
        for (std::size_t slot = 0; slot < kScenarios; ++slot) {
            const std::size_t scenario = round % 2 == 0 ? slot : kScenarios - 1 - slot;
            const auto sample = run_scenario(scenario, iterations, *counter);
            if (!sample) {
                std::cerr << "measurement failed: " << sample.error().message() << '\n';
                return 1;
            }
            results[scenario][round] = *sample;
            std::cout << round << ',' << sample->name << ',' << sample->ns_per_op << ','
                      << sample->checksum << '\n';
        }
    }
    print_summary(results);
    std::array<std::array<Result, kRounds>, 3> cache_results{};
    for (std::size_t round = 0; round < kRounds; ++round) {
        for (std::size_t slot = 0; slot < cache_results.size(); ++slot) {
            const std::size_t scenario = round % 2 == 0 ? slot : cache_results.size() - 1 - slot;
            const auto sample = run_cache(scenario, iterations, *counter);
            if (!sample) {
                std::cerr << "cache measurement failed: " << sample.error().message() << '\n';
                return 1;
            }
            if (sample->checksum == 0 || sample->ns_per_op <= 0) {
                return 1;
            }
            cache_results[scenario][round] = *sample;
            std::cout << round << ',' << sample->name << ',' << sample->ns_per_op << ','
                      << sample->checksum << '\n';
        }
    }
    print_summary(cache_results);
    const auto end = counter->read_end();
    if (end < origin) {
        std::cerr << "counter moved backward\n";
        return 1;
    }
    const auto elapsed = counter->to_nanoseconds(end - origin);
    if (!elapsed) {
        std::cerr << "counter conversion failed: " << elapsed.error().message() << '\n';
        return 1;
    }
    const auto reference = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - origin_reference);
    std::cout << "accuracy reference_ns=" << reference.count()
              << " counter_ns=" << elapsed->count()
              << " difference_ppm=" << (static_cast<double>(elapsed->count() - reference.count()) /
                                         static_cast<double>(reference.count()) * 1'000'000.0) << '\n';
    return 0;
}
