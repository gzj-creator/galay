#ifndef GALAY_BENCHMARK_ENVIRONMENT_H
#define GALAY_BENCHMARK_ENVIRONMENT_H

#include <galay/cpp/galay-utils/system/cpu.hpp>
#include <galay/cpp/galay-utils/system/numa.hpp>
#include <galay/cpp/galay-utils/system/memory.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace galay::benchmark {
namespace detail {

// Written once by main, before creating workers; subsequently read-only.
inline std::vector<unsigned> selectedCpus;
inline bool environmentInitialized = false;
inline bool cpuBindingEnabled = false;

inline std::expected<std::vector<unsigned>, std::error_code>
parse_placement_ids(std::string_view text, unsigned limit)
{
    std::vector<unsigned> ids;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto item = text.substr(0, comma);
        const auto dash = item.find('-');
        unsigned first = 0;
        unsigned last = 0;
        const auto begin = item.substr(0, dash);
        const auto parsed = std::from_chars(begin.data(), begin.data() + begin.size(), first);
        if (parsed.ec != std::errc{} || parsed.ptr != begin.data() + begin.size() || first >= limit) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        last = first;
        if (dash != std::string_view::npos) {
            const auto end = item.substr(dash + 1);
            const auto parsedEnd = std::from_chars(end.data(), end.data() + end.size(), last);
            if (parsedEnd.ec != std::errc{} || parsedEnd.ptr != end.data() + end.size() ||
                last >= limit || last < first) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
        }
        for (unsigned id = first; id <= last; ++id) {
            ids.push_back(id);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
        if (text.empty()) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
    }
    if (ids.empty()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

inline bool placement_error(std::string_view operation, const std::error_code& error)
{
    std::cerr << "GALAY_BENCH_ENV status=error operation=" << operation
              << " error=" << error.value() << " message=" << error.message() << '\n';
    return false;
}

inline void print_placement_ids(std::span<const unsigned> ids)
{
    for (std::size_t index = 0; index < ids.size(); ++index) {
        std::cerr << (index == 0 ? "" : ",") << ids[index];
    }
}

} // namespace detail

/**
 * @brief Call at the start of main, before workers, data allocation or timers.
 * GALAY_BENCH_CPUS: inherit (default), CPU list/ranges, or explicit none.
 * GALAY_BENCH_NUMA: keep (default), default, bind:<nodes>, interleave:<nodes>.
 * Any requested control that cannot be applied and read back fails startup.
 * @return 环境初始化成功时返回 true，否则返回 false
 */
[[nodiscard]] inline bool initialize_benchmark_environment()
{
    using utils::CPU;
    using utils::Numa;
    using utils::Memory;
    if (detail::environmentInitialized) {
        return detail::placement_error("already-initialized",
            std::make_error_code(std::errc::operation_not_permitted));
    }
    const char* cpuEnv = std::getenv("GALAY_BENCH_CPUS");
    const char* numaEnv = std::getenv("GALAY_BENCH_NUMA");
    const std::string_view cpuText = cpuEnv ? cpuEnv : "inherit";
    const std::string_view numaText = numaEnv ? numaEnv : "keep";
    const bool bindCpus = cpuText != "none";
    std::vector<unsigned> cpus;
    if (bindCpus) {
        const auto inherited = CPU::cpu_affinity();
        if (!inherited) {
            return detail::placement_error("read-cpu-affinity", inherited.error());
        }
        if (inherited->empty()) {
            return detail::placement_error("empty-cpu-affinity",
                std::make_error_code(std::errc::invalid_argument));
        }
        const auto requested = cpuText == "inherit"
            ? inherited : detail::parse_placement_ids(cpuText, inherited->back() + 1U);
        if (!requested) {
            return detail::placement_error("parse-GALAY_BENCH_CPUS", requested.error());
        }
        if (!std::includes(inherited->begin(), inherited->end(), requested->begin(), requested->end())) {
            return detail::placement_error("cpu-outside-inherited-mask",
                std::make_error_code(std::errc::invalid_argument));
        }
        cpus = *requested;
    }

    Memory::Policy numaMode = Memory::Policy::Default;
    std::vector<unsigned> numaNodes;
    const bool setNuma = numaText != "keep";
    if (setNuma && numaText != "default") {
        const auto colon = numaText.find(':');
        const auto mode = numaText.substr(0, colon);
        if (colon == std::string_view::npos || (mode != "bind" && mode != "interleave")) {
            return detail::placement_error("parse-GALAY_BENCH_NUMA",
                std::make_error_code(std::errc::invalid_argument));
        }
        numaMode = mode == "bind" ? Memory::Policy::Bind : Memory::Policy::Interleave;
        const auto parsed = detail::parse_placement_ids(numaText.substr(colon + 1), Numa::kMaxNodes);
        if (!parsed) {
            return detail::placement_error("parse-GALAY_BENCH_NUMA-nodes", parsed.error());
        }
        numaNodes = *parsed;
        const auto allowed = Numa::allowed_numa_nodes();
        if (!allowed) {
            return detail::placement_error("read-allowed-numa-nodes", allowed.error());
        }
        if (!std::includes(allowed->begin(), allowed->end(), numaNodes.begin(), numaNodes.end())) {
            return detail::placement_error("numa-outside-allowed-nodes",
                std::make_error_code(std::errc::invalid_argument));
        }
    }

    if (bindCpus) {
        const auto actual = CPU::bind_current_thread(cpus);
        if (!actual) {
            return detail::placement_error("bind-cpu-affinity", actual.error());
        }
        if (*actual != cpus) {
            return detail::placement_error("cpu-affinity-readback-mismatch",
                std::make_error_code(std::errc::state_not_recoverable));
        }
    }
    if (setNuma) {
        const auto applied = Memory::set_numa_policy(numaMode, numaNodes);
        if (!applied) {
            return detail::placement_error("set-numa-policy", applied.error());
        }
    }
    const auto policy = Memory::numa_policy();
    if (setNuma && !policy) {
        return detail::placement_error("read-numa-policy", policy.error());
    }
    if (setNuma && (policy->native_mode != static_cast<int>(numaMode) || policy->nodes != numaNodes)) {
        return detail::placement_error("numa-policy-readback-mismatch",
            std::make_error_code(std::errc::state_not_recoverable));
    }
    detail::selectedCpus = std::move(cpus);
    detail::cpuBindingEnabled = bindCpus;
    detail::environmentInitialized = true;
    std::cerr << "GALAY_BENCH_ENV status=ok affinity="
              << (!bindCpus ? "uncontrolled" : detail::selectedCpus.size() == 1 ? "pinned" : "cpu-set")
              << " cpus=";
    detail::print_placement_ids(detail::selectedCpus);
    std::cerr << " numa=" << numaText;
    if (policy) {
        std::cerr << " numa_mode=" << policy->native_mode << " numa_nodes=";
        detail::print_placement_ids(policy->nodes);
    } else {
        // Keep was requested, so observe without modifying or guessing policy.
        std::cerr << " numa_observed=unavailable numa_error=" << policy.error().value();
    }
    std::cerr << '\n';
    return true;
}

} // namespace galay::benchmark

#endif
