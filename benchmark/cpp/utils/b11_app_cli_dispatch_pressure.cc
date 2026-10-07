/**
 * @file b11_app_cli_dispatch_pressure.cc
 * @brief App 空参数帮助与版本短选项分派压力基准。
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-utils/app/app.hpp>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <ostream>
#include <streambuf>
#include <string>

namespace {

class NullBuffer final : public std::streambuf {
protected:
    int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
};

bool run_scenario(const char* name, galay::utils::App& app, int argc,
                 const char* const* argv, std::ostream& sink,
                 std::size_t iterations) {
    const auto start = std::chrono::steady_clock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        const int result = app.run(argc, argv, sink, sink);
        if (result != 0) {
            std::cerr << name << " failed at iteration " << i << '\n';
            return false;
        }
        ++completed;
    }
    const auto end = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    std::cout << name << ": " << completed << " runs, "
              << (static_cast<double>(completed) / seconds) << " ops/s\n";
    return completed == iterations;
}

} // namespace

int main() {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    constexpr std::size_t iterations = 100'000;

    galay::utils::App app("risk-control");
    galay::utils::App& configured = app.version("1.2.3", 'v');
    if (&configured != &app) {
        std::cerr << "version() did not return the configured app\n";
        return 1;
    }
    auto& config = app.opt<std::string>("config", 'c', "set config path");
    if (config.name() != "config") {
        std::cerr << "config option registration failed\n";
        return 1;
    }

    NullBuffer buffer;
    std::ostream sink(&buffer);
    const char* emptyArgv[] = {"risk-control"};
    const char* versionArgv[] = {"risk-control", "-v"};

    if (!run_scenario("BM_AppEmptyUsage", app, 1, emptyArgv, sink, iterations)) {
        return 1;
    }
    if (!run_scenario("BM_AppShortVersion", app, 2, versionArgv, sink, iterations)) {
        return 1;
    }
    return 0;
}
