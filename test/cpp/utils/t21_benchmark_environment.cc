#include "../../../benchmark/cpp/common/benchmark_environment.h"
#include "../../../benchmark/cpp/common/benchmark_affinity.h"

#include <array>
#include <iostream>
#include <thread>
#if defined(__linux__)
#include <cstddef>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#endif

using galay::utils::CPU;
using galay::utils::Numa;
using galay::utils::Memory;

int main()
{
#if !defined(__linux__)
    std::cout << "[SKIP] benchmark environment integration requires Linux\n";
    return 0;
#else
    const auto original = CPU::cpu_affinity();
    if (!original || original->empty()) {
        return 1;
    }
    const auto nodes = Numa::allowed_numa_nodes();
    std::array<bool, 3> numaAvailable{};
    bool probeValid = true;
    std::thread probe([&] {
        const std::array modes{galay::utils::Memory::Policy::Default,
                               galay::utils::Memory::Policy::Bind,
                               galay::utils::Memory::Policy::Interleave};
        for (std::size_t i = 0; i < modes.size(); ++i) {
            if (i != 0 && (!nodes || nodes->empty())) {
                continue;
            }
            const std::vector<unsigned> chosen = i == 0
                ? std::vector<unsigned>{} : std::vector<unsigned>{nodes->front()};
            const auto set = Memory::set_numa_policy(modes[i], chosen);
            if (!set) {
                probeValid = probeValid &&
                    (set.error() == std::errc::operation_not_permitted ||
                     set.error() == std::errc::permission_denied ||
                     set.error() == std::errc::function_not_supported);
                std::cout << "[SKIP] NUMA set probe: " << set.error().message() << '\n';
                continue;
            }
            numaAvailable[i] = Memory::numa_policy().has_value();
        }
    });
    probe.join();
    if (!probeValid) {
        return 1;
    }
    const std::string first = std::to_string(original->front());
    const std::string last = std::to_string(original->back());
    struct Case { const char* cpus; const char* numa; bool success; bool narrow; };
    const std::string selected = first + "," + last;
    const std::string range = first + "-" + first;
    const std::string bind = nodes && !nodes->empty()
        ? "bind:" + std::to_string(nodes->front()) : "bind:0";
    const std::string interleave = nodes && !nodes->empty()
        ? "interleave:" + std::to_string(nodes->front()) : "interleave:0";
    const Case cases[] = {
        {nullptr, nullptr, true, false},
        {"inherit", "keep", true, false},
        {"none", "keep", true, false},
        {first.c_str(), "keep", true, false},
        {selected.c_str(), "keep", true, false},
        {range.c_str(), "keep", true, false},
        {first.c_str(), "keep", true, true},
        {selected.c_str(), "keep", original->size() == 1, true},
        {"", "keep", false, false},
        {"-1", "keep", false, false},
        {"1,", "keep", false, false},
        {"1,,2", "keep", false, false},
        {"2-1", "keep", false, false},
        {"1-2-3", "keep", false, false},
        {"4294967296", "keep", false, false},
        {first.c_str(), "bind:", false, false},
        {first.c_str(), "default:0", false, false},
        {first.c_str(), "interleave:-1", false, false},
        {first.c_str(), "wrong", false, false},
        {first.c_str(), "bind:1024", false, false},
        {first.c_str(), bind.c_str(), numaAvailable[1], false},
        {first.c_str(), interleave.c_str(), numaAvailable[2], false},
        {first.c_str(), "default", numaAvailable[0], false},
    };
    for (const auto& test : cases) {
        const pid_t pid = fork();
        if (pid < 0) {
            return 1;
        }
        if (pid == 0) {
            if (unsetenv("GALAY_BENCH_CPUS") != 0 || unsetenv("GALAY_BENCH_NUMA") != 0 ||
                (test.cpus && setenv("GALAY_BENCH_CPUS", test.cpus, 1) != 0) ||
                (test.numa && setenv("GALAY_BENCH_NUMA", test.numa, 1) != 0)) {
                _exit(2);
            }
            if (test.narrow) {
                const std::array<unsigned, 1> cpus{original->front()};
                if (!CPU::bind_current_thread(cpus)) {
                    _exit(3);
                }
            }
            const bool ok = galay::benchmark::initialize_benchmark_environment();
            if (ok != test.success) {
                _exit(4);
            }
            if (ok) {
                const auto actual = CPU::cpu_affinity();
                bool inherited = false;
                std::thread child([&] { inherited = CPU::cpu_affinity() == actual; });
                child.join();
                if (!inherited) {
                    _exit(5);
                }
                if (test.cpus == selected.c_str() && !test.narrow) {
                    if (galay::benchmark::pin_current_thread(0) !=
                        galay::benchmark::ThreadPlacement::kPinnedToCore) {
                        _exit(6);
                    }
                    bool mapped = false;
                    std::thread worker([&] {
                        const auto placement = galay::benchmark::pin_current_thread(1);
                        const auto mask = CPU::cpu_affinity();
                        mapped = placement == galay::benchmark::ThreadPlacement::kPinnedToCore &&
                                 mask && *mask == std::vector<unsigned>{original->back()};
                    });
                    worker.join();
                    if (!mapped) {
                        _exit(7);
                    }
                }
            }
            _exit(0);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::cerr << "case failed: cpus=" << (test.cpus ? test.cpus : "unset")
                      << " numa=" << (test.numa ? test.numa : "unset")
                      << " status=" << status << '\n';
            return 1;
        }
    }
    // Deny one syscall only in disposable children: test real EPERM handling
    // without requiring root or changing host/container security settings.
    for (const int denied : {SYS_sched_setaffinity, SYS_set_mempolicy, SYS_get_mempolicy}) {
        const pid_t pid = fork();
        if (pid < 0) {
            return 1;
        }
        if (pid == 0) {
            if (setenv("GALAY_BENCH_CPUS", first.c_str(), 1) != 0 ||
                setenv("GALAY_BENCH_NUMA", "default", 1) != 0) {
                _exit(2);
            }
            sock_filter filter[] = {
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned>(denied), 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            };
            sock_fprog program{static_cast<unsigned short>(std::size(filter)), filter};
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
                prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
                std::cerr << "[SKIP] seccomp fault injection unavailable, errno=" << errno << '\n';
                _exit(77);
            }
            if (denied == SYS_sched_setaffinity) {
                const auto bound = CPU::bind_current_thread(*original);
                if (bound || bound.error().value() != EPERM) {
                    _exit(3);
                }
            } else if (denied == SYS_set_mempolicy) {
                const auto set = Memory::set_numa_policy(galay::utils::Memory::Policy::Default, {});
                const auto restore = Memory::restore_numa_policy({});
                if (set || set.error().value() != EPERM ||
                    restore || restore.error().value() != EPERM ||
                    restore.error().category() != std::generic_category()) {
                    _exit(4);
                }
            } else {
                const auto policy = Memory::numa_policy();
                if (policy || policy.error().value() != EPERM) {
                    _exit(5);
                }
            }
            _exit(galay::benchmark::initialize_benchmark_environment() ? 6 : 0);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            (WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 77)) {
            std::cerr << "syscall failure was hidden: " << denied << '\n';
            return 1;
        }
    }
    if (CPU::cpu_affinity() != original) {
        return 1;
    }
    std::cout << "[PASS] benchmark startup validation / inheritance / worker mapping\n";
    return 0;
#endif
}
