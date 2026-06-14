/**
 * @file b15_header.cc
 * @brief HTTP Header 解析性能基准测试
 *
 * 测试场景：
 * 1. BM_ParseCommonHeaders - 全部常见 header（fast-path）
 * 2. BM_ParseRareHeaders - 全部罕见 header（slow-path）
 * 3. BM_ParseMixedHeaders - 混合场景（常见 + 罕见）
 * 4. BM_HeaderLookup_Common - 常见 header 查询性能（O(1) vs O(log n)）
 */

#include "http/protoc/http_header.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <numeric>
#include <algorithm>

using namespace galay::http;
using namespace std::chrono;

struct BenchmarkResult {
    std::string name;
    double avg_ns;
    double min_ns;
    double max_ns;
    double median_ns;
    size_t iterations;
    size_t items_per_iteration;
};

class BenchmarkRunner {
public:
    BenchmarkRunner(const std::string& name, size_t iterations = 100000, size_t items_per_iter = 1)
        : m_name(name), m_iterations(iterations), m_items_per_iteration(items_per_iter) {}

    template<typename Func>
    BenchmarkResult run(Func&& func) {
        std::vector<double> durations;
        durations.reserve(m_iterations);

        // Warmup
        for (size_t i = 0; i < m_iterations / 10; ++i) {
            func();
        }

        // Actual benchmark
        for (size_t i = 0; i < m_iterations; ++i) {
            auto start = high_resolution_clock::now();
            func();
            auto end = high_resolution_clock::now();
            durations.push_back(duration_cast<nanoseconds>(end - start).count());
        }

        // Calculate statistics
        std::sort(durations.begin(), durations.end());
        double sum = std::accumulate(durations.begin(), durations.end(), 0.0);

        BenchmarkResult result;
        result.name = m_name;
        result.avg_ns = sum / durations.size();
        result.min_ns = durations.front();
        result.max_ns = durations.back();
        result.median_ns = durations[durations.size() / 2];
        result.iterations = m_iterations;
        result.items_per_iteration = m_items_per_iteration;

        return result;
    }

private:
    std::string m_name;
    size_t m_iterations;
    size_t m_items_per_iteration;
};

void printResult(const BenchmarkResult& result) {
    std::cout << std::left << std::setw(35) << result.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(1) << result.avg_ns << " ns"
              << std::setw(12) << result.median_ns << " ns"
              << std::setw(12) << result.min_ns << " ns"
              << std::setw(12) << result.max_ns << " ns"
              << std::setw(12) << result.iterations << " iters";

    if (result.items_per_iteration > 1) {
        std::cout << std::setw(12) << (result.avg_ns / result.items_per_iteration) << " ns/item";
    }

    std::cout << std::endl;
}

void printHeader() {
    std::cout << std::string(120, '=') << std::endl;
    std::cout << "HTTP Header Parsing Performance Benchmark" << std::endl;
    std::cout << std::string(120, '=') << std::endl;
    std::cout << std::left << std::setw(35) << "Benchmark"
              << std::right << std::setw(12) << "Avg"
              << std::setw(12) << "Median"
              << std::setw(12) << "Min"
              << std::setw(12) << "Max"
              << std::setw(12) << "Iterations"
              << std::setw(12) << "Per Item"
              << std::endl;
    std::cout << std::string(120, '-') << std::endl;
}

// Benchmark 1: 解析全部常见 header（fast-path）
void BM_ParseCommonHeaders() {
    const char* request =
        "GET /index.html HTTP/1.1\r\n"
        "host: example.com\r\n"
        "user-agent: Mozilla/5.0\r\n"
        "accept: text/html\r\n"
        "accept-encoding: gzip, deflate\r\n"
        "connection: keep-alive\r\n"
        "content-length: 0\r\n"
        "\r\n";

    BenchmarkRunner runner("BM_ParseCommonHeaders", 100000);
    auto result = runner.run([&]() {
        HttpRequestHeader header;
        auto [err, consumed] = header.fromString(request);
        // DoNotOptimize equivalent
        asm volatile("" : : "r,m"(err) : "memory");
        asm volatile("" : : "r,m"(consumed) : "memory");
    });
    printResult(result);
}

// Benchmark 2: 解析全部罕见 header（slow-path）
void BM_ParseRareHeaders() {
    const char* request =
        "GET /index.html HTTP/1.1\r\n"
        "x-custom-1: value1\r\n"
        "x-custom-2: value2\r\n"
        "x-custom-3: value3\r\n"
        "x-forwarded-for: 1.2.3.4\r\n"
        "x-request-id: abc123\r\n"
        "\r\n";

    BenchmarkRunner runner("BM_ParseRareHeaders", 100000);
    auto result = runner.run([&]() {
        HttpRequestHeader header;
        auto [err, consumed] = header.fromString(request);
        asm volatile("" : : "r,m"(err) : "memory");
        asm volatile("" : : "r,m"(consumed) : "memory");
    });
    printResult(result);
}

// Benchmark 3: 解析混合 header（常见 + 罕见）
void BM_ParseMixedHeaders() {
    const char* request =
        "GET /api/data HTTP/1.1\r\n"
        "host: api.example.com\r\n"
        "content-type: application/json\r\n"
        "content-length: 256\r\n"
        "authorization: Bearer token123\r\n"
        "x-api-key: secret\r\n"
        "x-request-id: req-456\r\n"
        "user-agent: CustomClient/1.0\r\n"
        "\r\n";

    BenchmarkRunner runner("BM_ParseMixedHeaders", 100000);
    auto result = runner.run([&]() {
        HttpRequestHeader header;
        auto [err, consumed] = header.fromString(request);
        asm volatile("" : : "r,m"(err) : "memory");
        asm volatile("" : : "r,m"(consumed) : "memory");
    });
    printResult(result);
}

// Benchmark 4: 常见 header 查询性能（O(1) vs O(log n)）
void BM_HeaderLookup_Common() {
    HttpRequestHeader header;
    const char* request =
        "GET / HTTP/1.1\r\n"
        "host: example.com\r\n"
        "content-length: 100\r\n"
        "user-agent: test\r\n"
        "\r\n";
    header.fromString(request);

    BenchmarkRunner runner("BM_HeaderLookup_Common", 100000, 3);
    auto result = runner.run([&]() {
        auto host = header.headerPairs().getValue("host");
        auto length = header.headerPairs().getValue("content-length");
        auto ua = header.headerPairs().getValue("user-agent");
        asm volatile("" : : "r,m"(host) : "memory");
        asm volatile("" : : "r,m"(length) : "memory");
        asm volatile("" : : "r,m"(ua) : "memory");
    });
    printResult(result);
}

// Benchmark 5: 罕见 header 查询性能
void BM_HeaderLookup_Rare() {
    HttpRequestHeader header;
    const char* request =
        "GET / HTTP/1.1\r\n"
        "x-custom-1: value1\r\n"
        "x-custom-2: value2\r\n"
        "x-custom-3: value3\r\n"
        "\r\n";
    header.fromString(request);

    BenchmarkRunner runner("BM_HeaderLookup_Rare", 100000, 3);
    auto result = runner.run([&]() {
        auto v1 = header.headerPairs().getValue("x-custom-1");
        auto v2 = header.headerPairs().getValue("x-custom-2");
        auto v3 = header.headerPairs().getValue("x-custom-3");
        asm volatile("" : : "r,m"(v1) : "memory");
        asm volatile("" : : "r,m"(v2) : "memory");
        asm volatile("" : : "r,m"(v3) : "memory");
    });
    printResult(result);
}

// Benchmark 6: 大量 header 解析（压力测试）
void BM_ParseLargeHeaders() {
    const char* request =
        "GET /api/endpoint HTTP/1.1\r\n"
        "host: api.example.com\r\n"
        "user-agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n"
        "accept: application/json, text/plain, */*\r\n"
        "accept-language: en-US,en;q=0.9\r\n"
        "accept-encoding: gzip, deflate, br\r\n"
        "content-type: application/json\r\n"
        "content-length: 1024\r\n"
        "authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9\r\n"
        "cookie: session=abc123; user_id=456\r\n"
        "cache-control: no-cache\r\n"
        "connection: keep-alive\r\n"
        "referer: https://example.com/page\r\n"
        "x-request-id: req-789\r\n"
        "x-api-key: secret-key-123\r\n"
        "x-forwarded-for: 192.168.1.1\r\n"
        "x-forwarded-proto: https\r\n"
        "x-real-ip: 10.0.0.1\r\n"
        "x-custom-header-1: value1\r\n"
        "x-custom-header-2: value2\r\n"
        "x-custom-header-3: value3\r\n"
        "\r\n";

    BenchmarkRunner runner("BM_ParseLargeHeaders", 50000);
    auto result = runner.run([&]() {
        HttpRequestHeader header;
        auto [err, consumed] = header.fromString(request);
        asm volatile("" : : "r,m"(err) : "memory");
        asm volatile("" : : "r,m"(consumed) : "memory");
    });
    printResult(result);
}

// Benchmark 7: Response header 解析
void BM_ParseResponseHeaders() {
    const char* response =
        "HTTP/1.1 200 OK\r\n"
        "content-type: application/json\r\n"
        "content-length: 512\r\n"
        "cache-control: max-age=3600\r\n"
        "connection: keep-alive\r\n"
        "\r\n";

    BenchmarkRunner runner("BM_ParseResponseHeaders", 100000);
    auto result = runner.run([&]() {
        HttpResponseHeader header;
        auto [err, consumed] = header.fromString(response);
        asm volatile("" : : "r,m"(err) : "memory");
        asm volatile("" : : "r,m"(consumed) : "memory");
    });
    printResult(result);
}

int main() {
    printHeader();

    std::cout << "\n[Phase 1: Parsing Performance - Fast-path vs Slow-path]\n" << std::endl;
    BM_ParseCommonHeaders();
    BM_ParseRareHeaders();
    BM_ParseMixedHeaders();
    BM_ParseLargeHeaders();
    BM_ParseResponseHeaders();

    std::cout << "\n[Phase 2: Lookup Performance - O(1) vs O(log n)]\n" << std::endl;
    BM_HeaderLookup_Common();
    BM_HeaderLookup_Rare();

    std::cout << "\n" << std::string(120, '=') << std::endl;
    std::cout << "Benchmark completed successfully!" << std::endl;
    std::cout << std::string(120, '=') << std::endl;

    return 0;
}
