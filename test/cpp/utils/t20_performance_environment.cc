#include <galay/cpp/galay-utils/system/performance.hpp>

#include <array>
#include <iostream>
#include <limits>
#include <thread>

using galay::utils::Performance;
using galay::utils::NumaPolicy;

int main()
{
    const auto original = Performance::cpuAffinity();
#if !defined(__linux__)
    if (original || original.error() != std::errc::operation_not_supported) {
        return 1;
    }
    std::cout << "[SKIP] CPU / NUMA control requires Linux\n";
    return 0;
#else
    if (!original || original->empty()) {
        std::cerr << "cannot query initial CPU affinity\n";
        return 1;
    }
    const auto empty = Performance::bindCurrentThread({});
    const std::array<unsigned, 1> invalid{std::numeric_limits<unsigned>::max()};
    const auto invalidCpu = Performance::bindCurrentThread(invalid);
    if (empty || empty.error() != std::errc::invalid_argument ||
        invalidCpu || invalidCpu.error() != std::errc::invalid_argument ||
        Performance::cpuAffinity() != original) {
        std::cerr << "invalid CPU requests must fail without mutation\n";
        return 1;
    }

    bool threadOk = false;
    std::thread worker([&] {
        if (Performance::cpuAffinity() != original) {
            return;
        }
        const std::array<unsigned, 2> repeated{original->front(), original->front()};
        const auto bound = Performance::bindCurrentThread(repeated);
        const std::vector<unsigned> selected{original->front()};
        if (!bound || *bound != selected || Performance::cpuAffinity() != bound) {
            return;
        }
        bool inherited = false;
        std::thread child([&] {
            const auto actual = Performance::cpuAffinity();
            inherited = actual && *actual == selected;
        });
        child.join();
        const auto restored = Performance::bindCurrentThread(*original);
        threadOk = inherited && restored && *restored == *original;
    });
    worker.join();
    if (!threadOk || Performance::cpuAffinity() != original) {
        std::cerr << "thread binding, inheritance, isolation or restore failed\n";
        return 1;
    }

    const std::array<unsigned, 1> nodeZero{0};
    const auto emptyBind = Performance::setNumaPolicy(NumaPolicy::Bind, {});
    const auto emptyInterleave = Performance::setNumaPolicy(NumaPolicy::Interleave, {});
    const auto defaultWithNodes = Performance::setNumaPolicy(NumaPolicy::Default, nodeZero);
    const auto invalidNode = Performance::setNumaPolicy(NumaPolicy::Bind, invalid);
    const auto invalidMode = Performance::setNumaPolicy(static_cast<NumaPolicy>(-1), {});
    for (const auto* result : {&emptyBind, &emptyInterleave, &defaultWithNodes,
                               &invalidNode, &invalidMode}) {
        if (*result || result->error() != std::errc::invalid_argument) {
            std::cerr << "invalid NUMA input was not rejected\n";
            return 1;
        }
    }

    int nativeMode = 0;
    errno = 0;
    const long nativeResult = syscall(SYS_get_mempolicy, &nativeMode, nullptr,
                                      0UL, nullptr, 0UL);
    const int nativeError = errno;
    const auto policy = Performance::numaPolicy();
    if (nativeResult != 0) {
        if (policy || policy.error().value() != nativeError ||
            policy.error().category() != std::generic_category()) {
            std::cerr << "NUMA query lost the original errno\n";
            return 1;
        }
        std::cout << "[SKIP] NUMA policy roundtrip: " << policy.error().message()
                  << " (errno=" << nativeError << ")\n";
    } else {
        if (!policy || policy->native_mode != nativeMode) {
            return 1;
        }
        const auto allowed = Performance::allowedNumaNodes();
        if (!allowed || allowed->empty()) {
            return 1;
        }
        bool numaOk = false;
        bool numaRestricted = false;
        std::thread numaWorker([&] {
            for (const auto mode : {NumaPolicy::Bind, NumaPolicy::Interleave}) {
                const std::array<unsigned, 1> selected{allowed->front()};
                const auto set = Performance::setNumaPolicy(mode, selected);
                if (!set && (set.error() == std::errc::operation_not_permitted ||
                             set.error() == std::errc::permission_denied ||
                             set.error() == std::errc::function_not_supported)) {
                    std::cout << "[SKIP] NUMA set restricted: " << set.error().message() << '\n';
                    numaRestricted = true;
                    return;
                }
                const auto actual = Performance::numaPolicy();
                if (!set || !actual || actual->native_mode != static_cast<int>(mode) ||
                    actual->nodes != std::vector<unsigned>{allowed->front()}) {
                    return;
                }
                bool inherited = false;
                std::thread child([&] {
                    const auto childPolicy = Performance::numaPolicy();
                    inherited = childPolicy && childPolicy->native_mode == actual->native_mode &&
                                childPolicy->nodes == actual->nodes;
                });
                child.join();
                if (!inherited) {
                    return;
                }
            }
            const auto reset = Performance::setNumaPolicy(NumaPolicy::Default, {});
            const auto actual = Performance::numaPolicy();
            numaOk = reset && actual && actual->native_mode == MPOL_DEFAULT && actual->nodes.empty();
        });
        numaWorker.join();
        const auto unchanged = Performance::numaPolicy();
        if ((!numaOk && !numaRestricted) || !unchanged || unchanged->native_mode != policy->native_mode ||
            unchanged->nodes != policy->nodes) {
            return 1;
        }
    }
    std::cout << "[PASS] Performance affinity / NUMA contracts\n";
    return 0;
#endif
}
