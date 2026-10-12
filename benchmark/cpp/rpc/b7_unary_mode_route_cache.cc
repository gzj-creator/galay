#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-utils/system/rdtsc.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace galay::rpc;

namespace {

Task<void> noop(RpcContext&)
{
    co_return;
}

class RouteBenchService : public RpcService {
public:
    RouteBenchService() : RpcService("RouteBench")
    {
        register_unary_method("shared", noop);
        register_client_streaming_method("shared", noop);
        register_server_streaming_method("shared", noop);
        register_bidi_streaming_method("shared", noop);
        for (int i = 0; i < 64; ++i) {
            const std::string name = "method" + std::to_string(i);
            register_unary_method(name, noop);
        }
    }
};

struct Config {
    int iterations = 1000000;
    bool use_rdtsc = false;
};

uint64_t percentile(std::vector<uint64_t> values, double p)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = std::min(values.size() - 1,
                                  static_cast<size_t>((values.size() - 1) * p));
    return values[index];
}

std::expected<Config, std::error_code> parse_args(int argc, char** argv)
{
    Config config;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        const std::string_view option{argv[i]};
        const std::string_view value{argv[i + 1]};
        if (option == "-n") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), config.iterations);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                config.iterations <= 0) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
        } else if (option == "--clock" && (value == "steady" || value == "rdtsc")) {
            config.use_rdtsc = value == "rdtsc";
        } else {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    const auto parsed = parse_args(argc, argv);
    if (!parsed) {
        std::cerr << "usage: benchmark_rpc_unary_mode_route_cache -n <positive count> "
                     "--clock <steady|rdtsc>: " << parsed.error().message() << '\n';
        return 1;
    }
    const Config config = *parsed;
    std::optional<galay::utils::Rdtsc> counter;
    if (config.use_rdtsc) {
#if defined(__linux__)
        const auto affinity = galay::utils::CPU::cpu_affinity();
        if (!affinity || affinity->size() != 1) {
            std::cerr << "rdtsc measurements require one CPU in GALAY_BENCH_CPUS\n";
            return 1;
        }
#else
        std::cerr << "rdtsc measurements require a verified single-CPU affinity\n";
        return 1;
#endif
        const auto created = galay::utils::Rdtsc::create();
        if (!created) {
            std::cerr << "rdtsc initialization failed: " << created.error().message() << '\n';
            return 1;
        }
        counter = *created;
    }
    RouteBenchService service;
    std::vector<RpcCallMode> modes{
        RpcCallMode::UNARY,
        RpcCallMode::CLIENT_STREAMING,
        RpcCallMode::SERVER_STREAMING,
        RpcCallMode::BIDI_STREAMING
    };

    int ok = 0;
    int errors = 0;
    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(std::min(config.iterations, 10000)));

    const auto measure_routes = [&](auto&& read_start, auto&& read_end, auto&& elapsed_ns) {
        for (int i = 0; i < config.iterations; ++i) {
            const RpcCallMode mode = modes[static_cast<size_t>(i) % modes.size()];
            const auto sample_begin = read_start();
            RpcMethodHandler* handler = service.find_method("shared", mode);
            const auto sample_end = read_end();
            if (handler != nullptr) {
                ++ok;
            } else {
                ++errors;
            }
            if (samples.size() < 10000) {
                const auto elapsed = elapsed_ns(sample_begin, sample_end);
                if (!elapsed) {
                    std::cerr << "measurement failed: " << elapsed.error().message() << '\n';
                    return false;
                }
                samples.push_back(static_cast<uint64_t>(elapsed->count()));
            }
        }
        return true;
    };
    const auto begin = std::chrono::steady_clock::now();
    const bool measured = counter
        ? measure_routes([&] { return counter->read_start(); },
                         [&] { return counter->read_end(); },
                         [&](std::uint64_t start, std::uint64_t end)
                             -> std::expected<std::chrono::nanoseconds, std::error_code> {
                             if (end < start) {
                                 return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
                             }
                             return counter->to_nanoseconds(end - start);
                         })
        : measure_routes([] { return std::chrono::steady_clock::now(); },
                         [] { return std::chrono::steady_clock::now(); },
                         [](auto start, auto end)
                             -> std::expected<std::chrono::nanoseconds, std::error_code> {
                             return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
                         });
    if (!measured) {
        return 1;
    }

    for (int i = 0; i < 128; ++i) {
        if (service.find_method("method" + std::to_string(i % 64), RpcCallMode::CLIENT_STREAMING) != nullptr) {
            ++errors;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::max(0.000001,
        std::chrono::duration<double>(end - begin).count());
    const int total = ok + errors;

    std::cout << "rpc unary mode route cache"
              << " clock=" << (config.use_rdtsc ? "rdtsc" : "steady")
              << " iterations=" << config.iterations
              << " ok=" << ok
              << " errors=" << errors
              << " error_rate=" << std::fixed << std::setprecision(6)
              << (total == 0 ? 1.0 : static_cast<double>(errors) / total)
              << " lookup_per_s=" << std::setprecision(2) << (ok / seconds)
              << " p50_ns=" << percentile(samples, 0.50)
              << " p95_ns=" << percentile(samples, 0.95)
              << " p99_ns=" << percentile(samples, 0.99)
              << "\n";
    return errors == 0 ? 0 : 1;
}
