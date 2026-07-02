#include <galay/cpp/galay-etcd/base/etcd_internal.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

int main()
{
    const std::array<std::string, 5> endpoints = {
        "http://127.0.0.1:2379",
        "https://etcd.example.com:443/v3",
        "http://localhost",
        "http://[::1]:2379",
        "HTTPS://ETCD.EXAMPLE.COM:1234/path",
    };
    constexpr int64_t iterations = 1000000;

    uint64_t checksum = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iterations; ++i) {
        for (const auto& endpoint : endpoints) {
            auto parsed = galay::etcd::internal::parseEndpoint(endpoint);
            if (!parsed.has_value()) {
                std::cerr << "parseEndpoint failed: " << parsed.error() << '\n';
                return 1;
            }
            checksum += parsed->host.size();
            checksum += parsed->port;
            checksum += parsed->secure ? 1U : 0U;
            checksum += parsed->ipv6 ? 1U : 0U;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    const auto operations = iterations * static_cast<int64_t>(endpoints.size());

    std::cout << "Operations : " << operations << '\n';
    std::cout << "Elapsed us : " << elapsed_us << '\n';
    std::cout << "Ops/us     : " << (elapsed_us > 0 ? operations / elapsed_us : 0) << '\n';
    std::cout << "Checksum   : " << checksum << '\n';
    return 0;
}
