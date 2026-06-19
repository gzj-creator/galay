/**
 * @file b4_policy_defaults.cc
 * @brief MCP生产策略值类型默认构造 smoke benchmark。
 */

#include "galay-mcp/common/mcp_policy.h"

#include <chrono>
#include <cstddef>
#include <iostream>

int main()
{
    constexpr std::size_t iterations = 1'000'000;
    std::size_t checksum = 0;

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        galay::mcp::McpProductionPolicy policy;
        checksum += policy.transport.max_in_flight_requests;
        checksum += policy.http_auth.enabled() ? 1U : 0U;
    }
    const auto end = std::chrono::steady_clock::now();

    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    if (checksum == 0) {
        std::cerr << "invalid policy benchmark checksum\n";
        return 1;
    }

    std::cout << "MCP policy default construction iterations: " << iterations << '\n';
    std::cout << "Elapsed: " << elapsed_us << " us\n";
    std::cout << "Checksum: " << checksum << '\n';
    return 0;
}
