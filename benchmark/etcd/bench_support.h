#ifndef GALAY_ETCD_BENCHMARK_ASYNC_BENCHMARK_SUPPORT_H
#define GALAY_ETCD_BENCHMARK_ASYNC_BENCHMARK_SUPPORT_H

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace galay::etcd::benchmark
{

enum class AsyncBenchmarkMode {
    Put,
    Mixed,
};

struct AsyncBenchmarkArgs
{
    std::string endpoint = "http://127.0.0.1:2379";
    int workers = 8;
    int ops_per_worker = 500;
    int value_size = 64;
    int io_schedulers = 0;
    AsyncBenchmarkMode mode = AsyncBenchmarkMode::Put;
    int timeout_seconds = 120;
};

struct AsyncBenchmarkResult
{
    std::string endpoint;
    AsyncBenchmarkMode mode = AsyncBenchmarkMode::Put;
    int workers = 0;
    int ops_per_worker = 0;
    int value_size = 0;
    int io_schedulers = 0;
    int64_t success = 0;
    int64_t failure = 0;
    int64_t total_ops = 0;
    double duration_seconds = 0.0;
    double throughput = 0.0;
    std::vector<int64_t> latency_us;
    std::string first_error;
};

[[nodiscard]] std::expected<AsyncBenchmarkResult, std::string>
runAsyncBenchmark(const AsyncBenchmarkArgs& args);

[[nodiscard]] const char* toString(AsyncBenchmarkMode mode) noexcept;
[[nodiscard]] std::expected<AsyncBenchmarkMode, std::string> parseAsyncBenchmarkMode(const std::string& value);
[[nodiscard]] double percentile(std::vector<int64_t> samples_us, double p);

} // namespace galay::etcd::benchmark

#endif // GALAY_ETCD_BENCHMARK_ASYNC_BENCHMARK_SUPPORT_H
