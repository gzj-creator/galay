#include "../../../benchmark/cpp/common/benchmark_affinity.h"

#include <cstdlib>
#include <iostream>

int main()
{
    using galay::benchmark::ThreadPlacement;
    using galay::benchmark::is_thread_placement_valid;
    if (is_thread_placement_valid(ThreadPlacement::kUnsupported) ||
        is_thread_placement_valid(ThreadPlacement::kPinnedToCore)) {
        return 1;
    }
    if (setenv("GALAY_BENCH_CPUS", "none", 1) != 0 ||
        setenv("GALAY_BENCH_NUMA", "keep", 1) != 0 ||
        !galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }
    const auto actual = galay::benchmark::pin_current_thread(0);
    if (actual != ThreadPlacement::kUnsupported ||
        !is_thread_placement_valid(actual) ||
        is_thread_placement_valid(ThreadPlacement::kPinnedToCore) ||
        is_thread_placement_valid(ThreadPlacement::kPerformanceClassOnly) ||
        is_thread_placement_valid(ThreadPlacement::kAffinityHintOnly)) {
        return 1;
    }
    std::cout << "[PASS] explicit uncontrolled benchmark placement\n";
    return 0;
}
