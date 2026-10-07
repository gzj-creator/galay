#include <galay/cpp/galay-utils/system/cpu.hpp>
#include <galay/cpp/galay-utils/system/numa.hpp>
#include <galay/cpp/galay-utils/system/memory.hpp>

#include <filesystem>
#include <array>
#include <cstdlib>
#include <string_view>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

using galay::utils::CPU;
using galay::utils::Numa;
using galay::utils::Memory;

namespace {
bool check(bool ok, const char* message)
{
    if (!ok) {
        std::cerr << message << '\n';
    }
    return ok;
}

template<class T>
bool error_is(const std::expected<T, std::error_code>& result, std::errc error)
{
    return !result && result.error() == error && result.error().category() == std::generic_category();
}

bool test_parser()
{
    using galay::utils::detail::parse_system_ids;
    const std::pair<const char*, std::vector<unsigned>> valid[] = {
        {"0", {0}}, {"0-3", {0, 1, 2, 3}},
        {"0-3,8,10-11", {0, 1, 2, 3, 8, 10, 11}},
        {" \t 0 - 3 , 8 , 10 - 11 \n", {0, 1, 2, 3, 8, 10, 11}},
        {"2,0,2,0-1", {0, 1, 2}},
    };
    for (const auto& [text, expected] : valid) {
        const auto actual = parse_system_ids(text, 1024);
        if (!check(actual && *actual == expected, "cpulist parsing changed IDs")) {
            return false;
        }
    }
    for (const char* text : {"", " \n", ",0", "0,", "0,,2", "0-", "-1", "+1",
                             "3-1", "1-2-3", "1a", "1 2", "0;2", "0\n2"}) {
        if (!check(error_is(parse_system_ids(text, 1024), std::errc::invalid_argument),
                   "malformed cpulist must return invalid_argument")) {
            return false;
        }
    }
    for (const char* text : {"1024", "0-1024", "4294967295"}) {
        if (!check(error_is(parse_system_ids(text, 1024), std::errc::value_too_large),
                   "out-of-capacity cpulist must fail before expanding a range")) {
            return false;
        }
    }
    const auto empty = parse_system_ids(" \n", 1024, true);
    return check(empty && empty->empty() &&
                 error_is(parse_system_ids("4294967296", 1024), std::errc::result_out_of_range) &&
                 error_is(parse_system_ids("0-4294967296", 1024), std::errc::result_out_of_range),
                 "empty cpulist or integer overflow contract failed");
}

#if defined(__linux__)
bool write_text(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream file(path);
    if (!file) {
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    return !file.fail();
}

bool test_fixture(const std::filesystem::path& root)
{
    using galay::utils::detail::NumaTopology;
    const NumaTopology topology(root.string());
    if (!check(error_is(topology.online_nodes(), std::errc::no_such_file_or_directory),
               "missing sysfs must retain ENOENT")) {
        return false;
    }
    std::error_code error;
    for (const char* node : {"node0", "node2"}) {
        if (!std::filesystem::create_directory(root / node, error) || error) {
            return false;
        }
    }
    if (!write_text(root / "online", "0,2\n") ||
        !write_text(root / "node0/cpulist", "0-3,8,10-11\n") ||
        !write_text(root / "node2/cpulist", "\n") ||
        !write_text(root / "node0/distance", "10 32\n") ||
        !write_text(root / "node2/distance", "35 10\n")) {
        return false;
    }
    const auto online = topology.online_nodes();
    const auto cpus = topology.cpus_of_node(0);
    const auto emptyCpus = topology.cpus_of_node(2);
    const auto node = topology.node_of_cpu(11);
    const auto distance = topology.distance(0, 2);
    const auto reverse = topology.distance(2, 0);
    if (!check(online && *online == std::vector<unsigned>{0, 2} && cpus &&
               *cpus == std::vector<unsigned>{0, 1, 2, 3, 8, 10, 11} &&
               emptyCpus && emptyCpus->empty() && node && *node == 0 &&
               distance && *distance == 32 && reverse && *reverse == 35,
               "sparse node IDs, CPU-less node or distance column mapping failed")) {
        return false;
    }
    // Node CPU lists must use the same dynamic CPU ID range as affinity APIs.
    if (!write_text(root / "node2/cpulist", "2048\n")) {
        return false;
    }
    const auto largeCpu = topology.node_of_cpu(2048);
    if (!check(largeCpu && *largeCpu == 2, "NUMA topology truncated a CPU ID above 1023")) {
        return false;
    }
    if (!check(error_is(topology.cpus_of_node(1), std::errc::invalid_argument) &&
               error_is(topology.distance(1, 2), std::errc::invalid_argument) &&
               error_is(topology.distance(0, 1), std::errc::invalid_argument) &&
               error_is(topology.node_of_cpu(9), std::errc::no_such_device) &&
               error_is(topology.node_of_cpu(std::numeric_limits<unsigned>::max()),
                       std::errc::invalid_argument) &&
               error_is(topology.cpus_of_node(1024), std::errc::invalid_argument),
               "illegal CPU/node error contract failed")) {
        return false;
    }
    for (const char* text : {"", "10", "10 20 30", "10 x", "10 0", "10 -1"}) {
        if (!write_text(root / "node0/distance", text) ||
            !check(error_is(topology.distance(0, 2), std::errc::invalid_argument),
                   "malformed distance must fail explicitly")) {
            return false;
        }
    }
    if (!write_text(root / "node0/distance", "10 4294967296") ||
        !check(error_is(topology.distance(0, 2), std::errc::result_out_of_range),
               "distance integer overflow was lost")) {
        return false;
    }
    if (!std::filesystem::remove(root / "node0/distance", error) || error ||
        !check(error_is(topology.distance(0, 2), std::errc::no_such_file_or_directory),
               "missing distance must retain ENOENT")) {
        return false;
    }
    if (!std::filesystem::remove(root / "node0/cpulist", error) || error ||
        !check(error_is(topology.cpus_of_node(0), std::errc::no_such_file_or_directory),
               "missing cpulist must retain ENOENT") ||
        !std::filesystem::create_directory(root / "node0/cpulist", error) || error ||
        !check(error_is(topology.cpus_of_node(0), std::errc::is_a_directory),
               "read failure must retain EISDIR")) {
        return false;
    }
    for (const char* text : {"", " \n", "0,", "3-1"}) {
        if (!write_text(root / "online", text) ||
            !check(error_is(topology.online_nodes(), std::errc::invalid_argument),
                   "empty/malformed online-node list was accepted")) {
            return false;
        }
    }
    return write_text(root / "online", "0-1024") &&
        check(error_is(topology.online_nodes(), std::errc::value_too_large),
              "topology must not silently truncate the fixed NUMA node limit");
}
#endif

bool test_system()
{
    if (!check(CPU::count() == std::thread::hardware_concurrency(),
               "CPU::count must preserve the standard-library hardware hint")) {
        return false;
    }
    const auto page = Memory::page_size();
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    if (!check(page && *page > 0, "page size must be a positive byte count")) {
        return false;
    }
#else
    if (!error_is(page, std::errc::operation_not_supported)) {
        return false;
    }
#endif
    const auto cpus = CPU::online_cpus();
    const auto current = CPU::current_id();
    const auto nodes = Numa::online_nodes();
#if !defined(__linux__)
    const unsigned invalid = std::numeric_limits<unsigned>::max();
    const std::array<unsigned, 1> request{invalid};
    return check(error_is(cpus, std::errc::operation_not_supported) &&
                 error_is(current, std::errc::operation_not_supported) &&
                 error_is(CPU::cpu_affinity(), std::errc::operation_not_supported) &&
                 error_is(CPU::bind_current_thread(request), std::errc::operation_not_supported) &&
                 error_is(nodes, std::errc::operation_not_supported) &&
                 error_is(Numa::allowed_numa_nodes(), std::errc::operation_not_supported) &&
                 error_is(Numa::node_of_cpu(invalid), std::errc::operation_not_supported) &&
                 error_is(Numa::cpus_of_node(invalid), std::errc::operation_not_supported) &&
                 error_is(Numa::distance(invalid, invalid), std::errc::operation_not_supported) &&
                 error_is(Memory::numa_policy(), std::errc::operation_not_supported) &&
                 error_is(Memory::set_numa_policy(Memory::Policy::Default, {}),
                         std::errc::operation_not_supported) &&
                 error_is(Memory::restore_numa_policy({}), std::errc::operation_not_supported),
                 "non-Linux topology/control must be unsupported");
#else
    const auto affinity = CPU::cpu_affinity();
    if (!check(cpus && !cpus->empty() && current && affinity && !affinity->empty() &&
               std::binary_search(cpus->begin(), cpus->end(), *current) &&
               std::binary_search(affinity->begin(), affinity->end(), *current) &&
               std::includes(cpus->begin(), cpus->end(), affinity->begin(), affinity->end()),
               "online CPU IDs/current snapshot/actual affinity are inconsistent")) {
        return false;
    }
    if (!nodes) {
        if (nodes.error() == std::errc::no_such_file_or_directory) {
            std::cout << "[SKIP] Linux node sysfs is not mounted\n";
            return true;
        }
        return false;
    }
    if (nodes->empty()) {
        return false;
    }
    for (const unsigned node : *nodes) {
        const auto nodeCpus = Numa::cpus_of_node(node);
        const auto local = Numa::distance(node, node);
        if (!check(nodeCpus && local && *local > 0, "online node query failed")) {
            return false;
        }
        for (const unsigned cpu : *nodeCpus) {
            const auto owner = Numa::node_of_cpu(cpu);
            if (!check(owner && *owner == node, "CPU/node topology does not roundtrip")) {
                return false;
            }
        }
    }
    if (nodes->size() > 1) {
        const auto remote = Numa::distance(nodes->front(), nodes->back());
        if (!check(remote && *remote > 0, "cross-node distance failed")) {
            return false;
        }
    } else {
        std::cout << "[SKIP] cross-node assertions on a single-node host\n";
    }
    const auto allowed = Numa::allowed_numa_nodes();
    if (!allowed) {
        if (allowed.error() != std::errc::operation_not_permitted &&
            allowed.error() != std::errc::permission_denied &&
            allowed.error() != std::errc::function_not_supported) {
            return false;
        }
        std::cout << "[SKIP] allowed-node query restricted: " << allowed.error().message() << '\n';
    } else {
        if (!check(std::includes(nodes->begin(), nodes->end(),
                                 allowed->begin(), allowed->end()),
                   "cpuset allowed nodes must be online")) {
            return false;
        }
    }
    const unsigned invalid = std::numeric_limits<unsigned>::max();
    return check(error_is(Numa::node_of_cpu(invalid), std::errc::invalid_argument) &&
                 error_is(Numa::cpus_of_node(invalid), std::errc::invalid_argument) &&
                 error_is(Numa::distance(nodes->front(), invalid), std::errc::invalid_argument),
                 "public topology did not reject an out-of-range ID");
#endif
}
}

int main()
{
    if (!test_parser() || !test_system()) {
        return 1;
    }
#if defined(__linux__)
    char temporary[] = "/tmp/galay-utils-topology-XXXXXX";
    const char* created = mkdtemp(temporary);
    if (!created) {
        return 1;
    }
    const bool ok = test_fixture(created);
    std::error_code error;
    const auto removed = std::filesystem::remove_all(created, error);
    if (!ok || error || removed == 0) {
        return 1;
    }
#endif
    std::cout << "[PASS] CPU / NUMA topology, sysfs parsing and page size\n";
    return 0;
}
