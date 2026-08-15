/**
 * @file t170_benchmark_measurement_contract.cc
 * @brief 锁定 kernel benchmark 的统计口径与防优化边界。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool requireContains(std::string_view source,
                     std::string_view needle,
                     std::string_view message)
{
    if (source.find(needle) != std::string_view::npos) {
        return true;
    }
    std::cerr << "[T170] " << message << '\n';
    return false;
}

bool requireAbsent(std::string_view source,
                   std::string_view needle,
                   std::string_view message)
{
    if (source.find(needle) == std::string_view::npos) {
        return true;
    }
    std::cerr << "[T170] " << message << '\n';
    return false;
}

}  // namespace

int main()
{
    const std::filesystem::path root(GALAY_PROJECT_ROOT);
    const auto scheduler_source = readAll(
        root / "benchmark/cpp/kernel/b14_scheduler_wakeup_latency.cc");
    const auto udp_source = readAll(
        root / "benchmark/cpp/kernel/b6_udp_socket_throughput.cc");
    const auto ring_source = readAll(
        root / "benchmark/cpp/kernel/b10_ring_buffer_throughput.cc");
    const auto tcp_client_source = readAll(
        root / "benchmark/cpp/kernel/b3_tcp_client_throughput.cc");
    const auto libuv_server_source = readAll(
        root / "benchmark/c/kernel/b26_libuv_echo_server.c");

    if (scheduler_source.empty() || udp_source.empty() || ring_source.empty() ||
        tcp_client_source.empty() || libuv_server_source.empty()) {
        std::cerr << "[T170] failed to read benchmark sources\n";
        return 1;
    }

    bool ok = true;
    ok = requireAbsent(scheduler_source,
                       "[SkewedTwoSchedulerSteal]",
                       "IO scheduler benchmark must not report disabled stealing as a speedup") && ok;
    ok = requireContains(scheduler_source,
                         "[RoundRobinTwoScheduler]",
                         "IO scheduler benchmark must measure runtime round-robin scaling") && ok;
    ok = requireContains(scheduler_source,
                         "stealing=disabled",
                         "IO scheduler benchmark must state the reactor owner-affinity contract") && ok;

    ok = requireContains(udp_source,
                         "g_client_sent",
                         "UDP benchmark must keep client traffic counters separate") && ok;
    ok = requireContains(udp_source,
                         "g_server_received",
                         "UDP benchmark must keep server traffic counters separate") && ok;
    ok = requireContains(udp_source,
                         "measurement_end",
                         "UDP throughput must use the exact active measurement window") && ok;
    ok = requireAbsent(udp_source,
                       "MESSAGES_PER_CLIENT",
                       "duration-mode UDP benchmark must not stop at a fixed message ceiling") && ok;

    ok = requireContains(ring_source,
                         "RingBufferBackendStrategy::Vector",
                         "RingBuffer benchmark must exercise physical vector wrap-around") && ok;
    ok = requireContains(ring_source,
                         "logical_wraps",
                         "mmap RingBuffer benchmark must report logical rather than dual-iovec wraps") && ok;
    ok = requireContains(ring_source,
                         "actual_backend=vector-fallback",
                         "RingBuffer benchmark must disclose mmap fallback instead of reporting mmap data") && ok;
    ok = requireContains(ring_source,
                         "benchmarkBarrier",
                         "RingBuffer microbenchmarks must keep measured state observable") && ok;
    ok = requireContains(ring_source,
                         "benchmark_kind=single_thread_hot_cache",
                         "RingBuffer benchmark must disclose its hot-cache microbenchmark scope") && ok;
    ok = requireContains(ring_source,
                         "amortized ns/pair",
                         "RingBuffer benchmark must not present pipelined throughput as isolated latency") && ok;

    ok = requireContains(tcp_client_source,
                         "--io-schedulers",
                         "TCP client benchmark must expose IO scheduler count for symmetric comparisons") && ok;
    ok = requireContains(tcp_client_source,
                         "static_cast<std::size_t>(i) % schedulers.size()",
                         "TCP client benchmark must distribute connections across all IO schedulers") && ok;
    ok = requireContains(libuv_server_source,
                         "SO_REUSEPORT",
                         "libuv TCP baseline must support one listener per event-loop thread") && ok;
    ok = requireContains(libuv_server_source,
                         "uv_thread_create",
                         "libuv TCP baseline must run the requested event loops on separate threads") && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "T170-BenchmarkMeasurementContract PASS\n";
    return 0;
}
