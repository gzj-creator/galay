/**
 * @file t166_spsc_paired_final_drain_source.cc
 * @brief Ensure formal kernel comparison registration is Asio-only.
 */

#include "result_writer.h"

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
    if (!input.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool contains(std::string_view source, std::string_view needle, std::string_view message)
{
    if (source.find(needle) != std::string_view::npos) return true;
    std::cerr << "[T166] " << message << '\n';
    return false;
}

bool absent(std::string_view source, std::string_view needle, std::string_view message)
{
    if (source.find(needle) == std::string_view::npos) return true;
    std::cerr << "[T166] " << message << '\n';
    return false;
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t166_spsc_paired_final_drain_source");
    writer.add_test();

    const std::filesystem::path root(GALAY_PROJECT_ROOT);
    const auto cmake = read_all(root / "benchmark/cpp/kernel/CMakeLists.txt");
    const auto asio = read_all(
        root / "benchmark/cpp/kernel/compare/boost-asio-coro/boost_asio_coro_udp.cc");
    const auto tcp_asio = read_all(
        root / "benchmark/cpp/kernel/compare/boost-asio-coro/boost_asio_coro_tcp.cc");
    const auto tcp_galay = read_all(
        root / "benchmark/cpp/kernel/b31_tcp_socket_fair_throughput.cc");
    if (cmake.empty() || asio.empty() || tcp_asio.empty() || tcp_galay.empty()) {
        std::cerr << "[T166] failed to read formal benchmark sources\n";
        return 1;
    }

    bool ok = true;
    ok = contains(cmake,
                  "benchmark_kernel_compare_boost_asio_coro_udp",
                  "kernel CMake must register the Boost.Asio coroutine baseline") && ok;
    ok = contains(cmake,
                  "benchmark_kernel_compare_boost_asio_coro_tcp",
                  "kernel CMake must register the Boost.Asio TCP coroutine baseline") && ok;
    ok = absent(cmake,
                "benchmark_kernel_compare_mpsc_paired",
                "Crossbeam paired targets must not be formal benchmark targets") && ok;
    ok = absent(cmake,
                "benchmark_kernel_compare_spsc_paired",
                "legacy SPSC paired targets must not be formal benchmark targets") && ok;
    ok = contains(asio, "clients_ready", "Asio baseline must expose a readiness barrier") && ok;
    ok = contains(asio, "wait_for_completion", "Asio baseline must await coroutine completion") && ok;
    ok = contains(asio, "runtime_errors", "Asio baseline must separate runtime errors") && ok;
    ok = contains(asio, "shutdown_errors", "Asio baseline must separate shutdown errors") && ok;
    ok = contains(tcp_galay, "read_exact", "Galay TCP baseline must read complete stream frames") && ok;
    ok = contains(tcp_galay, "write_all", "Galay TCP baseline must write complete stream frames") && ok;
    ok = contains(tcp_galay, "settled_counters_match", "Galay TCP baseline must gate on settled counters") && ok;
    ok = contains(tcp_asio, "co_spawn", "Boost.Asio TCP baseline must use co_spawn") && ok;
    ok = contains(tcp_asio, "async_read", "Boost.Asio TCP baseline must use async_read") && ok;
    ok = contains(tcp_asio, "async_write", "Boost.Asio TCP baseline must use async_write") && ok;
    ok = contains(tcp_asio, "settled_counters_match", "Boost.Asio TCP baseline must gate on settled counters") && ok;

    if (!ok) {
        writer.add_failed();
        writer.write_result();
        return 1;
    }
    writer.add_passed();
    writer.write_result();
    std::cout << "T166-AsioComparisonPolicy PASS\n";
    return 0;
}
