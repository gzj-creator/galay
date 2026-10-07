#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-tracing/log/logger.h>
#include <galay/cpp/galay-tracing/log/log_sink.h>

#include <chrono>
#include <iostream>
#include <memory>

namespace {

class NullSink final : public galay::tracing::LogSink {
public:
    void write(const galay::tracing::LogRecord&) override {
        ++count;
    }

    std::size_t count{0};
};

[[nodiscard]] const char* build_type() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

} // namespace

int main() {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    constexpr int kIterations = 100000;
    auto sink = std::make_shared<NullSink>();
    galay::tracing::Logger logger;
    logger.clear_sinks();
    logger.add_sink(sink);
    logger.set_level(galay::tracing::LogLevel::kInfo);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        logger.log(galay::tracing::LogLevel::kInfo, {"benchmark/b2_enabled_log.cc", 0, "main"}, "value {}", i);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    std::cout << "B2-EnabledLog workload=" << kIterations << " build=" << build_type()
              << " backend=core ns_per_log=" << (static_cast<double>(ns) / kIterations)
              << " sink_writes=" << sink->count << '\n';
}
