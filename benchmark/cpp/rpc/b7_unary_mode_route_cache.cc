#include <galay/cpp/galay-rpc/kernel/rpc_service.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
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
        registerUnaryMethod("shared", noop);
        registerClientStreamingMethod("shared", noop);
        registerServerStreamingMethod("shared", noop);
        registerBidiStreamingMethod("shared", noop);
        for (int i = 0; i < 64; ++i) {
            const std::string name = "method" + std::to_string(i);
            registerUnaryMethod(name, noop);
        }
    }
};

struct Config {
    int iterations = 1000000;
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

Config parseArgs(int argc, char** argv)
{
    Config config;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "-n") {
            config.iterations = std::stoi(argv[i + 1]);
        }
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    Config config = parseArgs(argc, argv);
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

    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < config.iterations; ++i) {
        const RpcCallMode mode = modes[static_cast<size_t>(i) % modes.size()];
        const auto sample_begin = std::chrono::steady_clock::now();
        RpcMethodHandler* handler = service.findMethod("shared", mode);
        const auto sample_end = std::chrono::steady_clock::now();
        if (handler != nullptr) {
            ++ok;
        } else {
            ++errors;
        }
        if (samples.size() < 10000) {
            samples.push_back(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(sample_end - sample_begin).count()));
        }
    }

    for (int i = 0; i < 128; ++i) {
        if (service.findMethod("method" + std::to_string(i % 64), RpcCallMode::CLIENT_STREAMING) != nullptr) {
            ++errors;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::max(0.000001,
        std::chrono::duration<double>(end - begin).count());
    const int total = ok + errors;

    std::cout << "rpc unary mode route cache"
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
