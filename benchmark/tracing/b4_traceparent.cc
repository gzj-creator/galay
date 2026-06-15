#include "galay-tracing/context/traceparent.h"

#include <chrono>
#include <iostream>

namespace {

[[nodiscard]] const char* buildType() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

} // namespace

int main() {
    constexpr int kIterations = 200000;
    constexpr auto kHeader = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

    const auto start = std::chrono::steady_clock::now();
    std::size_t bytes = 0;
    for (int i = 0; i < kIterations; ++i) {
        auto context = galay::tracing::extractTraceparent(kHeader);
        if (!context.has_value()) {
            return 1;
        }
        bytes += galay::tracing::injectTraceparent(*context).size();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    std::cout << "B4-Traceparent workload=" << kIterations << " build=" << buildType()
              << " backend=core ns_per_parse_inject=" << (static_cast<double>(ns) / kIterations)
              << " bytes=" << bytes << '\n';
}
