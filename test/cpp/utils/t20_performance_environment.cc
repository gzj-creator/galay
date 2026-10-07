#include <galay/cpp/galay-utils/system/cpu.hpp>
#include <galay/cpp/galay-utils/system/numa.hpp>
#include <galay/cpp/galay-utils/system/memory.hpp>

#include <array>
#include <iostream>
#include <limits>
#include <thread>

using galay::utils::CPU;
using galay::utils::Numa;
using galay::utils::Memory;

int main()
{
    const auto original = CPU::cpu_affinity();
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
    const auto empty = CPU::bind_current_thread({});
    const std::array<unsigned, 1> invalid{std::numeric_limits<unsigned>::max()};
    const auto invalidCpu = CPU::bind_current_thread(invalid);
    if (empty || empty.error() != std::errc::invalid_argument ||
        invalidCpu || invalidCpu.error() != std::errc::invalid_argument ||
        CPU::cpu_affinity() != original) {
        std::cerr << "invalid CPU requests must fail without mutation\n";
        return 1;
    }

    bool threadOk = false;
    std::thread worker([&] {
        if (CPU::cpu_affinity() != original) {
            return;
        }
        const std::array<unsigned, 2> repeated{original->front(), original->front()};
        const auto bound = CPU::bind_current_thread(repeated);
        const std::vector<unsigned> selected{original->front()};
        if (!bound || *bound != selected || CPU::cpu_affinity() != bound) {
            return;
        }
        bool inherited = false;
        std::thread child([&] {
            const auto actual = CPU::cpu_affinity();
            inherited = actual && *actual == selected;
        });
        child.join();
        const auto restored = CPU::bind_current_thread(*original);
        threadOk = inherited && restored && *restored == *original;
    });
    worker.join();
    if (!threadOk || CPU::cpu_affinity() != original) {
        std::cerr << "thread binding, inheritance, isolation or restore failed\n";
        return 1;
    }

    const std::array<unsigned, 1> nodeZero{0};
    const auto emptyBind = Memory::set_numa_policy(Memory::Policy::Bind, {});
    const auto emptyInterleave = Memory::set_numa_policy(Memory::Policy::Interleave, {});
    const auto defaultWithNodes = Memory::set_numa_policy(Memory::Policy::Default, nodeZero);
    const auto invalidNode = Memory::set_numa_policy(Memory::Policy::Bind, invalid);
    const auto invalidMode = Memory::set_numa_policy(static_cast<Memory::Policy>(-1), {});
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
    const auto policy = Memory::numa_policy();
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
        const auto allowed = Numa::allowed_numa_nodes();
        if (!allowed || allowed->empty()) {
            return 1;
        }
        bool numaOk = false;
        bool numaRestricted = false;
        std::thread numaWorker([&] {
            const auto initial = Memory::numa_policy();
            if (!initial) {
                return;
            }
            for (const auto mode : {Memory::Policy::Bind, Memory::Policy::Interleave}) {
                const std::array<unsigned, 1> selected{allowed->front()};
                const auto set = Memory::set_numa_policy(mode, selected);
                if (!set && (set.error() == std::errc::operation_not_permitted ||
                             set.error() == std::errc::permission_denied ||
                             set.error() == std::errc::function_not_supported)) {
                    std::cout << "[SKIP] NUMA set restricted: " << set.error().message() << '\n';
                    numaRestricted = true;
                    return;
                }
                const auto actual = Memory::numa_policy();
                if (!set || !actual || actual->native_mode != static_cast<int>(mode) ||
                    actual->nodes != std::vector<unsigned>{allowed->front()}) {
                    return;
                }
                bool inherited = false;
                std::thread child([&] {
                    const auto childPolicy = Memory::numa_policy();
                    inherited = childPolicy && childPolicy->native_mode == actual->native_mode &&
                                childPolicy->nodes == actual->nodes;
                });
                child.join();
                if (!inherited) {
                    return;
                }
            }
            const std::vector<Memory::PolicyState> nativePolicies{
                {MPOL_PREFERRED, {allowed->front()}},
                {MPOL_PREFERRED, {}}, {MPOL_LOCAL, {}},
                {MPOL_BIND | MPOL_F_STATIC_NODES, {allowed->front()}},
                {MPOL_INTERLEAVE | MPOL_F_RELATIVE_NODES, {0}},
#if defined(MPOL_F_NUMA_BALANCING)
                {MPOL_BIND | MPOL_F_NUMA_BALANCING, {allowed->front()}},
#endif
            };
            for (const auto& requested : nativePolicies) {
                const auto set = Memory::restore_numa_policy(requested);
                if (!set && set.error() == std::errc::invalid_argument &&
                    (requested.native_mode & MPOL_MODE_FLAGS) != 0) {
                    // Optional flags can be absent on an older running kernel.
                    std::cout << "[SKIP] native mempolicy flags unsupported, mode="
                              << requested.native_mode << '\n';
                    continue;
                }
                const auto saved = Memory::numa_policy();
                // Linux canonicalizes PREFERRED with an empty mask to LOCAL.
                const int expectedMode = requested.native_mode == MPOL_PREFERRED &&
                    requested.nodes.empty() ? MPOL_LOCAL : requested.native_mode;
                if (!set || !saved || saved->native_mode != expectedMode ||
                    saved->nodes != requested.nodes) {
                    return;
                }
                const auto changed = Memory::set_numa_policy(Memory::Policy::Default, {});
                const auto defaultPolicy = Memory::numa_policy();
                if (!changed || !defaultPolicy || defaultPolicy->native_mode != MPOL_DEFAULT ||
                    !defaultPolicy->nodes.empty()) {
                    return;
                }
                const auto restored = Memory::restore_numa_policy(*saved);
                const auto actual = Memory::numa_policy();
                if (!restored || !actual || actual->native_mode != saved->native_mode ||
                    actual->nodes != saved->nodes) {
                    return;
                }
                bool inherited = false;
                std::thread child([&] {
                    const auto childPolicy = Memory::numa_policy();
                    inherited = childPolicy && childPolicy->native_mode == saved->native_mode &&
                                childPolicy->nodes == saved->nodes;
                });
                child.join();
                if (!inherited) {
                    return;
                }
            }
            const auto reset = Memory::restore_numa_policy(*initial);
            const auto actual = Memory::numa_policy();
            numaOk = reset && actual && actual->native_mode == initial->native_mode &&
                     actual->nodes == initial->nodes;
        });
        numaWorker.join();
        const auto unchanged = Memory::numa_policy();
        if ((!numaOk && !numaRestricted) || !unchanged || unchanged->native_mode != policy->native_mode ||
            unchanged->nodes != policy->nodes) {
            return 1;
        }
    }
    std::cout << "[PASS] CPU affinity / NUMA memory contracts\n";
    return 0;
#endif
}
