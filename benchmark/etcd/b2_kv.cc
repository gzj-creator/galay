#include "bench_support.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

int parsePositiveInt(const char* value, int fallback)
{
    if (value == nullptr) {
        return fallback;
    }

    const int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

} // namespace

int main(int argc, char** argv)
{
    galay::etcd::benchmark::AsyncBenchmarkArgs args;
    if (argc > 1) args.endpoint = argv[1];
    if (argc > 2) args.workers = parsePositiveInt(argv[2], args.workers);
    if (argc > 3) args.ops_per_worker = parsePositiveInt(argv[3], args.ops_per_worker);
    if (argc > 4) args.value_size = parsePositiveInt(argv[4], args.value_size);
    if (argc > 5) {
        auto mode = galay::etcd::benchmark::parseAsyncBenchmarkMode(argv[5]);
        if (!mode.has_value()) {
            std::cerr << mode.error() << '\n';
            return 1;
        }
        args.mode = *mode;
    }
    if (argc > 6) args.io_schedulers = parsePositiveInt(argv[6], args.io_schedulers);

    auto result = galay::etcd::benchmark::runAsyncBenchmark(args);
    if (!result.has_value()) {
        std::cerr << "async benchmark failed: " << result.error() << '\n';
        return 1;
    }

    std::cout << "Endpoint      : " << result->endpoint << '\n';
    std::cout << "Mode          : " << galay::etcd::benchmark::toString(result->mode) << '\n';
    std::cout << "Workers       : " << result->workers << '\n';
    std::cout << "Ops/worker    : " << result->ops_per_worker << '\n';
    std::cout << "Value size    : " << result->value_size << " bytes\n";
    std::cout << "IO schedulers : " << result->io_schedulers << '\n';
    std::cout << "Total ops     : " << result->total_ops << '\n';
    std::cout << "Success       : " << result->success << '\n';
    std::cout << "Failure       : " << result->failure << '\n';
    std::cout << "Duration      : " << result->duration_seconds << " s\n";
    std::cout << "Throughput    : " << result->throughput << " ops/s\n";
    std::cout << "Latency p50   : " << galay::etcd::benchmark::percentile(result->latency_us, 0.50) << " us\n";
    std::cout << "Latency p95   : " << galay::etcd::benchmark::percentile(result->latency_us, 0.95) << " us\n";
    std::cout << "Latency p99   : " << galay::etcd::benchmark::percentile(result->latency_us, 0.99) << " us\n";
    std::cout << "Latency max   : "
              << (result->latency_us.empty()
                      ? 0
                      : *std::max_element(result->latency_us.begin(), result->latency_us.end()))
              << " us\n";
    if (!result->first_error.empty()) {
        std::cout << "First error   : " << result->first_error << '\n';
    }

    return result->failure == 0 ? 0 : 2;
}
