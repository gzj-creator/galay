#include <galay/cpp/galay-utils/system/memory.hpp>

#include <cstdarg>
#include <iostream>
#include <limits>

using galay::utils::Memory;

#if defined(__linux__)
namespace {
Memory::PolicyState nativeState;
int setError = 0;
int getError = 0;
unsigned setCalls = 0;
long pageResult = 4096;
int pageError = 0;
}

extern "C" long __wrap_sysconf(int name)
{
    if (name != _SC_PAGESIZE) {
        errno = EINVAL;
        return -1;
    }
    errno = pageError;
    return pageResult;
}

// Verify the Linux ABI boundary independently of the machine's NUMA layout,
// cpuset and set_mempolicy permissions; no actual thread policy is changed.
extern "C" long __wrap_syscall(long number, ...)
{
    va_list args;
    va_start(args, number);
    if (number == SYS_set_mempolicy) {
        ++setCalls;
        const int mode = va_arg(args, int);
        const auto* mask = va_arg(args, unsigned long*);
        const auto maxnode = va_arg(args, unsigned long);
        va_end(args);
        if (setError != 0) {
            errno = setError;
            return -1;
        }
        nativeState.native_mode = mode;
        nativeState.nodes.clear();
        // Linux get_nodes() decrements maxnode, then get_bitmap() clears the
        // unused final bits. maxnode 1024 would therefore lose node ID 1023.
        for (unsigned node = 0; mask && node + 1UL < maxnode; ++node) {
            if (mask[node / (sizeof(unsigned long) * 8)] &
                (1UL << (node % (sizeof(unsigned long) * 8)))) {
                nativeState.nodes.push_back(node);
            }
        }
        return 0;
    }
    if (number == SYS_get_mempolicy) {
        auto* mode = va_arg(args, int*);
        auto* mask = va_arg(args, unsigned long*);
        const auto maxnode = va_arg(args, unsigned long);
        va_end(args);
        if (getError != 0) {
            errno = getError;
            return -1;
        }
        if (mode) {
            *mode = nativeState.native_mode;
        }
        if (mask) {
            for (const unsigned node : nativeState.nodes) {
                if (node >= maxnode) {
                    errno = EINVAL;
                    return -1;
                }
                mask[node / (sizeof(unsigned long) * 8)] |=
                    1UL << (node % (sizeof(unsigned long) * 8));
            }
        }
        return 0;
    }
    va_end(args);
    errno = ENOSYS;
    return -1;
}
#endif

int main()
{
#if !defined(__linux__)
    std::cout << "[SKIP] Linux mempolicy/sysconf ABI injection\n";
#else
    const Memory::PolicyState policies[] = {
        {MPOL_DEFAULT, {}}, {MPOL_LOCAL, {}}, {MPOL_PREFERRED, {}},
        {MPOL_PREFERRED, {2}}, {MPOL_BIND | MPOL_F_STATIC_NODES, {0, 2}},
        {MPOL_INTERLEAVE | MPOL_F_RELATIVE_NODES, {0, 2}},
        {MPOL_BIND, {Memory::kMaxNodes - 1}},
#if defined(MPOL_F_NUMA_BALANCING)
        {MPOL_BIND | MPOL_F_NUMA_BALANCING | MPOL_F_STATIC_NODES, {2}},
#endif
    };
    for (const auto& initial : policies) {
        nativeState = initial;
        const auto saved = Memory::numa_policy();
        const auto changed = Memory::set_numa_policy(Memory::Policy::Default, {});
        if (!saved || !changed || nativeState.native_mode != MPOL_DEFAULT ||
            !nativeState.nodes.empty()) {
            return 1;
        }
        const auto restored = Memory::restore_numa_policy(*saved);
        const auto actual = Memory::numa_policy();
        if (!restored || !actual || actual->native_mode != initial.native_mode ||
            actual->nodes != initial.nodes) {
            std::cerr << "native mode/flags/nodemask were not restored through the Linux ABI\n";
            return 1;
        }
    }
    const Memory::PolicyState invalid[] = {
        {-1, {}}, {MPOL_MAX, {0}}, {MPOL_BIND | (1 << 20), {0}},
        {MPOL_DEFAULT, {0}}, {MPOL_LOCAL, {0}}, {MPOL_BIND, {}}, {MPOL_INTERLEAVE, {}},
        {MPOL_PREFERRED | MPOL_F_STATIC_NODES, {}},
        {MPOL_LOCAL | MPOL_F_RELATIVE_NODES, {}},
        {MPOL_BIND | MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES, {0}},
        {MPOL_BIND, {Memory::kMaxNodes}},
        {MPOL_PREFERRED, {std::numeric_limits<unsigned>::max()}},
    };
    for (const auto& state : invalid) {
        const auto before = setCalls;
        const auto result = Memory::restore_numa_policy(state);
        if (result || result.error() != std::errc::invalid_argument || setCalls != before) {
            std::cerr << "invalid saved state reached set_mempolicy\n";
            return 1;
        }
    }
    for (const int error : {EPERM, EINVAL, ENOMEM, ENOSYS}) {
        setError = error;
        const auto result = Memory::restore_numa_policy(policies[4]);
        if (result || result.error().value() != error ||
            result.error().category() != std::generic_category()) {
            std::cerr << "restore failure lost the kernel errno\n";
            return 1;
        }
    }
    getError = EIO;
    const auto query = Memory::numa_policy();
    if (query || query.error().value() != EIO ||
        query.error().category() != std::generic_category()) {
        return 1;
    }
    const auto page = Memory::page_size();
    if (!page || *page != 4096) {
        return 1;
    }
    pageResult = -1;
    pageError = EACCES;
    const auto failed = Memory::page_size();
    if (failed || failed.error().value() != EACCES ||
        failed.error().category() != std::generic_category()) {
        return 1;
    }
    for (const long invalidSize : {-1L, 0L}) {
        pageResult = invalidSize;
        pageError = 0;
        const auto unknown = Memory::page_size();
        if (unknown || unknown.error() != std::errc::io_error) {
            return 1;
        }
    }
    std::cout << "[PASS] native mempolicy mode/flags, highest node bit and syscall failures\n";
#endif
    return 0;
}
