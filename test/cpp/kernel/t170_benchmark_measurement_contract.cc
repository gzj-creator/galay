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

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool require_contains(std::string_view source,
                     std::string_view needle,
                     std::string_view message)
{
    if (source.find(needle) != std::string_view::npos) {
        return true;
    }
    std::cerr << "[T170] " << message << '\n';
    return false;
}

bool require_absent(std::string_view source,
                   std::string_view needle,
                   std::string_view message)
{
    if (source.find(needle) == std::string_view::npos) {
        return true;
    }
    std::cerr << "[T170] " << message << '\n';
    return false;
}

std::string_view slice_between(std::string_view source,
                              std::string_view begin_marker,
                              std::string_view end_marker)
{
    const auto begin = source.find(begin_marker);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = source.find(end_marker, begin + begin_marker.size());
    if (end == std::string_view::npos) {
        return {};
    }
    return source.substr(begin, end - begin);
}

}  // namespace

int main()
{
    const std::filesystem::path root(GALAY_PROJECT_ROOT);
    const auto scheduler_source = read_all(
        root / "benchmark/cpp/kernel/b14_scheduler_wakeup_latency.cc");
    const auto udp_source = read_all(
        root / "benchmark/cpp/kernel/b6_udp_socket_throughput.cc");
    const auto ring_source = read_all(
        root / "benchmark/cpp/kernel/b10_ring_buffer_throughput.cc");
    const auto tcp_client_source = read_all(
        root / "benchmark/cpp/kernel/b3_tcp_client_throughput.cc");
    const auto asio_source = read_all(
        root / "benchmark/cpp/kernel/compare/boost-asio-coro/boost_asio_coro_udp.cc");
    const auto tcp_source = read_all(
        root / "benchmark/cpp/kernel/b31_tcp_socket_fair_throughput.cc");
    const auto asio_tcp_source = read_all(
        root / "benchmark/cpp/kernel/compare/boost-asio-coro/boost_asio_coro_tcp.cc");

    if (scheduler_source.empty() || udp_source.empty() || ring_source.empty() ||
        tcp_client_source.empty() || asio_source.empty() || tcp_source.empty() ||
        asio_tcp_source.empty()) {
        std::cerr << "[T170] failed to read benchmark sources\n";
        return 1;
    }

    const auto udp_server_worker = slice_between(
        udp_source, "Task<void> udp_server_worker", "Task<void> udp_benchmark_client");
    const auto udp_client = slice_between(
        udp_source, "Task<void> udp_benchmark_client", "void print_benchmark_results");
    if (udp_server_worker.empty() || udp_client.empty()) {
        std::cerr << "[T170] failed to locate UDP worker source boundaries\n";
        return 1;
    }

    bool ok = true;
    ok = require_absent(scheduler_source,
                       "[SkewedTwoSchedulerSteal]",
                       "IO scheduler benchmark must not report disabled stealing as a speedup") && ok;
    ok = require_contains(scheduler_source,
                         "[RoundRobinTwoScheduler]",
                         "IO scheduler benchmark must measure runtime round-robin scaling") && ok;
    ok = require_contains(scheduler_source,
                         "stealing=disabled",
                         "IO scheduler benchmark must state the reactor owner-affinity contract") && ok;

    ok = require_contains(udp_source,
                         "g_client_sent",
                         "UDP benchmark must keep client traffic counters separate") && ok;
    ok = require_contains(udp_source,
                         "g_server_received",
                         "UDP benchmark must keep server traffic counters separate") && ok;
    ok = require_contains(udp_source,
                         "measurement_end",
                         "UDP throughput must use the exact active measurement window") && ok;
    ok = require_contains(udp_source,
                         "wait_for_clients_ready",
                         "Galay UDP benchmark must wait for all clients before warmup") && ok;
    ok = require_contains(udp_source,
                         "enum class Phase",
                         "Galay must expose the same warmup/measurement/drain lifecycle") && ok;
    ok = require_contains(udp_source,
                         "kMeasuredMarker",
                         "Galay must tag the measured request set in the payload") && ok;
    ok = require_contains(udp_server_worker,
                         "buffer[0] == kMeasuredMarker",
                         "Galay server counters must follow the tagged measured request set") && ok;
    ok = require_contains(udp_client,
                         "recv_buffer[0] == kMeasuredMarker",
                         "Galay client replies must follow the tagged measured request set") && ok;
    ok = require_contains(udp_source,
                         "g_phase.store(Phase::drain",
                         "Galay server must remain active during the drain phase") && ok;
    ok = require_contains(udp_source,
                         "std::this_thread::sleep_for(CLIENT_DRAIN_TIMEOUT)",
                         "Galay must use the fixed formal drain window") && ok;
    ok = require_contains(udp_source,
                         "Settled measured packets:",
                         "Galay raw output must expose settled measured counters") && ok;
    ok = require_contains(udp_source,
                         "settled_counters_match",
                         "Galay benchmark status must validate settled counter alignment") && ok;
    ok = require_absent(udp_source,
                       "g_running",
                       "Galay shutdown must not stop the server before client drain") && ok;
    ok = require_absent(udp_server_worker,
                       "mark_client_startup_failed()",
                       "server startup failures must not change client readiness") && ok;
    ok = require_contains(udp_client,
                         "if (!socket_result) {\n        add_counter(g_errors);\n        mark_client_startup_failed();",
                         "client socket creation failures must complete the readiness barrier") && ok;
    ok = require_contains(udp_server_worker,
                         "char buffer[MESSAGE_SIZE]",
                         "Galay server coroutine buffer must match the measured payload") && ok;
    ok = require_contains(udp_client,
                         "char recv_buffer[MESSAGE_SIZE]",
                         "Galay client coroutine buffer must match the measured payload") && ok;
    ok = require_absent(udp_source,
                       "[65536]",
                       "Galay UDP coroutine frames must not reserve oversized datagram buffers") && ok;
    ok = require_absent(udp_source,
                       "MESSAGES_PER_CLIENT",
                       "duration-mode UDP benchmark must not stop at a fixed message ceiling") && ok;
    ok = require_contains(asio_source,
                         "settled_counters_match",
                         "Boost.Asio benchmark status must validate settled counter alignment") && ok;
    ok = require_contains(asio_source,
                         "settled_loss_pct",
                         "Boost.Asio raw output must expose settled loss") && ok;

    ok = require_contains(tcp_source,
                         "enum class Phase",
                         "Galay TCP benchmark must expose the warmup/measurement/drain lifecycle") && ok;
    ok = require_contains(tcp_source,
                         "settled_counters_match",
                         "Galay TCP benchmark status must validate settled counter alignment") && ok;
    ok = require_contains(tcp_source,
                         "read_exact",
                         "Galay TCP benchmark must account for stream partial reads") && ok;
    ok = require_contains(tcp_source,
                         "write_all",
                         "Galay TCP benchmark must account for stream partial writes") && ok;
    ok = require_contains(asio_tcp_source,
                         "co_spawn",
                         "Boost.Asio TCP baseline must use co_spawn") && ok;
    ok = require_contains(asio_tcp_source,
                         "async_read",
                         "Boost.Asio TCP baseline must use coroutine async_read") && ok;
    ok = require_contains(asio_tcp_source,
                         "async_write",
                         "Boost.Asio TCP baseline must use coroutine async_write") && ok;
    ok = require_contains(asio_tcp_source,
                         "settled_counters_match",
                         "Boost.Asio TCP benchmark status must validate settled counters") && ok;

    ok = require_contains(ring_source,
                         "RingBufferBackendStrategy::Vector",
                         "RingBuffer benchmark must exercise physical vector wrap-around") && ok;
    ok = require_contains(ring_source,
                         "logical_wraps",
                         "mmap RingBuffer benchmark must report logical rather than dual-iovec wraps") && ok;
    ok = require_contains(ring_source,
                         "actual_backend=vector-fallback",
                         "RingBuffer benchmark must disclose mmap fallback instead of reporting mmap data") && ok;
    ok = require_contains(ring_source,
                         "benchmark_barrier",
                         "RingBuffer microbenchmarks must keep measured state observable") && ok;
    ok = require_contains(ring_source,
                         "benchmark_kind=single_thread_hot_cache",
                         "RingBuffer benchmark must disclose its hot-cache microbenchmark scope") && ok;
    ok = require_contains(ring_source,
                         "amortized ns/pair",
                         "RingBuffer benchmark must not present pipelined throughput as isolated latency") && ok;

    ok = require_contains(tcp_client_source,
                         "--io-schedulers",
                         "TCP client benchmark must expose IO scheduler count for symmetric comparisons") && ok;
    ok = require_contains(tcp_client_source,
                         "static_cast<std::size_t>(i) % schedulers.size()",
                         "TCP client benchmark must distribute connections across all IO schedulers") && ok;
    ok = require_contains(asio_source,
                         "co_spawn",
                         "Boost.Asio baseline must use co_spawn coroutine execution") && ok;
    ok = require_contains(asio_source,
                         "awaitable",
                         "Boost.Asio baseline must use awaitable coroutine I/O") && ok;
    ok = require_contains(asio_source,
                         "clients_ready",
                         "Boost.Asio baseline must wait for all clients before measurement") && ok;
    ok = require_contains(asio_source,
                         "wait_for_completion",
                         "Boost.Asio baseline must wait for coroutine shutdown") && ok;
    ok = require_contains(asio_source,
                         "async_send_to",
                         "Boost.Asio UDP baseline must use unconnected send-to like Galay") && ok;
    ok = require_contains(asio_source,
                         "async_receive_from",
                         "Boost.Asio UDP baseline must use receive-from like Galay") && ok;
    ok = require_absent(asio_source,
                       "async_connect",
                       "Boost.Asio UDP baseline must not use a connected-socket fast path") && ok;
    ok = require_absent(asio_source,
                       "std::shared_ptr<udp::socket>",
                       "Boost.Asio hot paths must not copy shared socket ownership") && ok;
    ok = require_contains(asio_source,
                         "std::vector<udp::socket> server_sockets",
                         "Boost.Asio server sockets must have direct state ownership") && ok;
    ok = require_contains(asio_source,
                         "receive_with_timeout(\n    udp::socket& socket",
                         "Boost.Asio receive helper must borrow its socket") && ok;
    ok = require_contains(asio_source,
                         "std::vector<char> buffer(state.config.payload_bytes)",
                         "Boost.Asio server buffer must match the configured payload") && ok;
    ok = require_contains(asio_source,
                         "std::vector<char> payload(state.config.payload_bytes)",
                         "Boost.Asio client payload must match the configured size") && ok;
    ok = require_contains(asio_source,
                         "std::vector<char> response(state.config.payload_bytes)",
                         "Boost.Asio client response buffer must match the configured size") && ok;
    for (const std::string_view counter : {
             "client_bytes_sent",
             "client_bytes_received",
             "server_bytes_received",
             "server_bytes_sent",
         }) {
        ok = require_contains(asio_source,
                             counter,
                             "Boost.Asio must match Galay packet and byte instrumentation") && ok;
    }
    ok = require_contains(asio_source,
                         "void add_counter(std::atomic<std::uint64_t>& counter",
                         "Boost.Asio must use the same saturating traffic counter shape") && ok;
    ok = require_absent(asio_source,
                       "std::array<char, kMaxPayloadBytes>",
                       "Boost.Asio coroutine frames must not reserve maximum-size buffers") && ok;
    ok = require_contains(asio_source,
                         "make_address(\"127.0.0.1\", error)",
                         "Boost.Asio address parsing must report errors explicitly") && ok;
    ok = require_contains(asio_source,
                         "kServerPort = 9090",
                         "Boost.Asio must use the same UDP endpoint as Galay") && ok;
    ok = require_contains(asio_source,
                         "kMeasuredMarker",
                         "Boost.Asio must tag the measured request set in the payload") && ok;
    ok = require_contains(asio_source,
                         "buffer[0] == kMeasuredMarker",
                         "Boost.Asio server counters must follow the tagged measured request set") && ok;
    ok = require_contains(asio_source,
                         "response[0] == kMeasuredMarker",
                         "Boost.Asio client replies must follow the tagged measured request set") && ok;
    ok = require_contains(asio_source,
                         "if (phase == Phase::drain)",
                         "Boost.Asio clients must drain the tagged measured request set") && ok;
    ok = require_contains(asio_source,
                         "settled client_sent=",
                         "Boost.Asio raw output must expose settled measured counters") && ok;
    ok = require_contains(asio_source,
                         "if (error == asio::error::operation_aborted) return;",
                         "expected Boost.Asio shutdown cancellation must not be an error") && ok;

    ok = require_absent(tcp_source,
                       "GALAY_TCP_LATENCY_HIST",
                       "Galay TCP benchmark must not retain temporary environment instrumentation") && ok;
    ok = require_absent(tcp_source,
                       "hist ",
                       "Galay TCP benchmark must not emit temporary latency histograms") && ok;
    ok = require_absent(asio_tcp_source,
                       "GALAY_TCP_LATENCY_HIST",
                       "Boost.Asio TCP benchmark must not retain temporary environment instrumentation") && ok;
    ok = require_absent(asio_tcp_source,
                       "diag ",
                       "Boost.Asio TCP benchmark must not emit temporary diagnostic counters") && ok;
    ok = require_contains(tcp_source,
                         "constexpr auto kRecvTimeout = std::chrono::milliseconds(10)",
                         "Galay TCP benchmark receive timeout must retain the 10ms workload") && ok;
    ok = require_contains(asio_tcp_source,
                         "constexpr std::chrono::milliseconds kRecvTimeout{10}",
                         "Boost.Asio TCP benchmark receive timeout must retain the 10ms workload") && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "T170-BenchmarkMeasurementContract PASS\n";
    return 0;
}
