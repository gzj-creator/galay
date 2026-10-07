#include <galay/cpp/galay-utils/system/cpu.hpp>
#include <galay/cpp/galay-utils/system/process.hpp>
#include "../../../benchmark/cpp/common/benchmark_environment.h"

#include <iostream>
#include <limits>
#include <array>

#if defined(__linux__)
namespace {
std::vector<unsigned> kernelCpus;
std::size_t kernelMaskSize = 0;
int queryError = 0;
int bindError = 0;
int currentError = 0;
unsigned queryCalls = 0;
unsigned smallMaskQueries = 0;
}

// A synthetic kernel mask makes sparse and > CPU_SETSIZE IDs testable on
// ordinary machines. Linker wrapping leaves the process's real mask unchanged.
extern "C" int __wrap_sched_getaffinity(pid_t, std::size_t size, cpu_set_t* mask)
{
    ++queryCalls;
    if (size < kernelMaskSize) {
        ++smallMaskQueries;
    }
    if (queryError != 0 || size < kernelMaskSize) {
        errno = queryError != 0 ? queryError : EINVAL;
        return -1;
    }
    CPU_ZERO_S(size, mask);
    for (const unsigned cpu : kernelCpus) {
        CPU_SET_S(cpu, size, mask);
    }
    return 0;
}

extern "C" int __wrap_sched_setaffinity(pid_t, std::size_t size, const cpu_set_t* mask)
{
    if (bindError != 0) {
        errno = bindError;
        return -1;
    }
    std::vector<unsigned> selected;
    for (const unsigned cpu : kernelCpus) {
        if (CPU_ISSET_S(cpu, size, mask)) {
            selected.push_back(cpu);
        }
    }
    if (selected.empty()) {
        errno = EINVAL;
        return -1;
    }
    kernelCpus = std::move(selected);
    return 0;
}

extern "C" int __wrap_sched_getcpu()
{
    if (currentError != 0) {
        errno = currentError;
        return -1;
    }
    return static_cast<int>(kernelCpus.front());
}
#endif

int main()
{
#if !defined(__linux__)
    std::cout << "[SKIP] synthetic Linux affinity mask\n";
#else
    const unsigned hint = galay::utils::CPU::count();
    if (hint >= std::numeric_limits<int>::max() - CPU_SETSIZE - 1U) {
        return 1;
    }
    for (const unsigned high : {hint + 1U, hint + CPU_SETSIZE + 1U}) {
        kernelCpus = {0, high};
        kernelMaskSize = CPU_ALLOC_SIZE(static_cast<std::size_t>(high) + 1);
        const auto process = galay::utils::Process::cpu_affinity();
        if (!process || *process != kernelCpus) {
            std::cerr << "Process truncated a sparse mask at CPU::count()/CPU_SETSIZE\n";
            return 1;
        }
        const auto current = galay::utils::CPU::cpu_affinity();
        if (!current || *current != kernelCpus) {
            return 1;
        }
        const std::array<unsigned, 1> selected{high};
        const auto set = galay::utils::Process::set_cpu_affinity(selected);
        const auto bound = galay::utils::CPU::bind_current_thread(selected);
        const auto after = galay::utils::Process::cpu_affinity();
        if (!set || !bound || *bound != std::vector<unsigned>{high} ||
            !after || *after != *bound) {
            std::cerr << "A valid sparse CPU ID above the hardware hint was rejected\n";
            return 1;
        }
    }
    if (queryCalls < 4 || smallMaskQueries == 0) {
        return 1;
    }
    const std::string chosen = std::to_string(kernelCpus.front());
    if (setenv("GALAY_BENCH_CPUS", chosen.c_str(), 1) != 0 ||
        setenv("GALAY_BENCH_NUMA", "keep", 1) != 0 ||
        !galay::benchmark::initialize_benchmark_environment() ||
        galay::benchmark::detail::selectedCpus != kernelCpus) {
        std::cerr << "benchmark rejected a valid inherited CPU ID above 1023\n";
        return 1;
    }
    queryError = EACCES;
    const auto failedQuery = galay::utils::CPU::cpu_affinity();
    if (failedQuery || failedQuery.error().value() != EACCES ||
        failedQuery.error().category() != std::generic_category()) {
        return 1;
    }
    queryError = ESRCH;
    const auto missing = galay::utils::Process::cpu_affinity(123);
    if (missing || missing.error() != galay::utils::ProcessAffinityError::NotFound) {
        return 1;
    }
    queryError = 0;
    bindError = EPERM;
    const auto denied = galay::utils::CPU::bind_current_thread(kernelCpus);
    if (denied || denied.error().value() != EPERM ||
        denied.error().category() != std::generic_category()) {
        return 1;
    }
    const auto current = galay::utils::CPU::current_id();
    if (!current || *current != kernelCpus.front()) {
        return 1;
    }
    currentError = ENOSYS;
    const auto unavailable = galay::utils::CPU::current_id();
    if (unavailable || unavailable.error().value() != ENOSYS ||
        unavailable.error().category() != std::generic_category()) {
        return 1;
    }
    std::cout << "[PASS] sparse CPU IDs, dynamic mask probing and errno propagation\n";
#endif
    return 0;
}
