#include "../common/benchmark_environment.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

#include <galay/cpp/galay-utils/system/process.hpp>

int main(int argc, char** argv) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    int iterations = 1000;
    if (argc > 1) {
        iterations = std::max(1, std::atoi(argv[1]));
    }

    auto original = galay::utils::Process::cpu_affinity();
    if (!original.has_value()) {
        if (original.error() == galay::utils::ProcessAffinityError::Unsupported) {
            std::cout << "[SKIP] process affinity unsupported\n";
            return 125;
        }
        std::cerr << "cpuAffinity failed: "
                  << galay::utils::process_affinity_error_string(original.error()) << '\n';
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto set_result = galay::utils::Process::set_cpu_affinity(*original);
        if (!set_result.has_value()) {
            std::cerr << "setCpuAffinity failed: "
                      << galay::utils::process_affinity_error_string(set_result.error()) << '\n';
            return 2;
        }

        auto current = galay::utils::Process::cpu_affinity();
        if (!current.has_value() || *current != *original) {
            std::cerr << "affinity roundtrip mismatch\n";
            return 3;
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    std::cout << "process_affinity_roundtrip iterations=" << iterations
              << " total_us=" << micros
              << " avg_us=" << (static_cast<double>(micros) / iterations) << '\n';
    return 0;
}
