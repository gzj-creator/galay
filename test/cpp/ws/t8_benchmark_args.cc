/**
 * @file t8_benchmark_args.cc
 * @brief 验证 WS benchmark client 的 URL 参数解析。
 */

#include "benchmark/cpp/ws/ws_benchmark_args.h"

#include <cassert>
#include <string>

int main()
{
    char arg0[] = "benchmark_ws_ws_client_throughput";
    char clients[] = "100";
    char duration[] = "10";
    char payload[] = "1024";
    char url[] = "ws://127.0.0.1:18080/ws";
    char nodelay_off[] = "off";
    char nodelay_false[] = "false";
    char nodelay_zero[] = "0";
    char nodelay_on[] = "on";
    char nodelay_true[] = "true";
    char nodelay_one[] = "1";
    char nodelay_invalid[] = "maybe";

    char* default_argv[] = {arg0, clients, duration, payload};
    assert(galay::benchmark::ws::resolveBenchmarkClientUrl(4, default_argv) ==
           std::string(galay::benchmark::ws::kDefaultBenchmarkClientUrl));
    assert(galay::benchmark::ws::resolveBenchmarkServerNoDelay(4, default_argv, 4));

    char* custom_argv[] = {arg0, clients, duration, payload, url};
    assert(galay::benchmark::ws::resolveBenchmarkClientUrl(5, custom_argv) == std::string(url));
    assert(galay::benchmark::ws::resolveBenchmarkServerNoDelay(5, custom_argv, 4));

    char* off_argv[] = {arg0, clients, duration, nodelay_off};
    assert(!galay::benchmark::ws::resolveBenchmarkServerNoDelay(4, off_argv, 3));

    char* false_argv[] = {arg0, clients, duration, nodelay_false};
    assert(!galay::benchmark::ws::resolveBenchmarkServerNoDelay(4, false_argv, 3));

    char* zero_argv[] = {arg0, clients, duration, nodelay_zero};
    assert(!galay::benchmark::ws::resolveBenchmarkServerNoDelay(4, zero_argv, 3));

    char* on_argv[] = {arg0, clients, duration, nodelay_on};
    assert(galay::benchmark::ws::resolveBenchmarkServerNoDelay(4, on_argv, 3));

    char* true_argv[] = {arg0, clients, duration, nodelay_true};
    assert(galay::benchmark::ws::resolveBenchmarkServerNoDelay(4, true_argv, 3));

    char* one_argv[] = {arg0, clients, duration, nodelay_one};
    assert(galay::benchmark::ws::resolveBenchmarkServerNoDelay(4, one_argv, 3));

    char* invalid_argv[] = {arg0, clients, duration, nodelay_invalid};
    assert(galay::benchmark::ws::resolveBenchmarkServerNoDelay(4, invalid_argv, 3));

    return 0;
}
