/**
 * @file b9_hpack.cc
 * @brief HPACK 编解码微基准测试
 * @details 单独测量 HPACK encode/decode 吞吐，排除网络 IO 开销
 */

#include "galay-http2/protoc/http2_hpack.h"
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <numeric>

using namespace galay::http2;

// 模拟典型 HTTP/2 请求头
static std::vector<Http2HeaderField> makeRequestHeaders(int variant) {
    std::vector<Http2HeaderField> h;
    h.push_back({":method", "GET"});
    h.push_back({":scheme", "https"});
    h.push_back({":authority", "www.example.com"});
    h.push_back({":path", "/api/v1/resource/" + std::to_string(variant)});
    h.push_back({"accept", "application/json"});
    h.push_back({"accept-encoding", "gzip, deflate"});
    h.push_back({"accept-language", "en-US,en;q=0.9,zh-CN;q=0.8"});
    h.push_back({"user-agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)"});
    h.push_back({"cache-control", "no-cache"});
    h.push_back({"x-request-id", "req-" + std::to_string(variant) + "-abcdef1234567890"});
    return h;
}

// 模拟典型 HTTP/2 响应头
static std::vector<Http2HeaderField> makeResponseHeaders(int variant) {
    std::vector<Http2HeaderField> h;
    h.push_back({":status", "200"});
    h.push_back({"content-type", "application/json; charset=utf-8"});
    h.push_back({"content-length", std::to_string(256 + variant)});
    h.push_back({"server", "galay-http2/1.0"});
    h.push_back({"date", "Sat, 08 Feb 2026 12:00:00 GMT"});
    h.push_back({"cache-control", "max-age=3600"});
    h.push_back({"x-trace-id", "trace-" + std::to_string(variant) + "-0987654321fedcba"});
    return h;
}

struct BenchResult {
    double encode_ops;   // ops/s
    double decode_ops;   // ops/s
    double encode_ns;    // ns per header-block
    double decode_ns;    // ns per header-block
    size_t total_encoded_bytes;
};

BenchResult runBench(int iterations) {
    // 预生成头部列表（模拟不同请求）
    constexpr int kVariants = 256;
    std::vector<std::vector<Http2HeaderField>> req_headers(kVariants);
    std::vector<std::vector<Http2HeaderField>> resp_headers(kVariants);
    for (int i = 0; i < kVariants; i++) {
        req_headers[i] = makeRequestHeaders(i);
        resp_headers[i] = makeResponseHeaders(i);
    }

    BenchResult result{};

    // ---- Encode benchmark ----
    {
        size_t total_bytes = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; i++) {
            HpackEncoder encoder;
            // 每个 encoder 实例编码一批请求+响应，模拟一个连接的生命周期
            for (int v = 0; v < kVariants; v++) {
                std::string encoded = encoder.encode(req_headers[v]);
                total_bytes += encoded.size();
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        double elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        double total_ops = static_cast<double>(iterations) * kVariants;
        result.encode_ops = total_ops / (elapsed_ns / 1e9);
        result.encode_ns = elapsed_ns / total_ops;
        result.total_encoded_bytes = total_bytes;
    }

    // ---- Decode benchmark ----
    // 先编码一批数据用于解码
    std::vector<std::string> encoded_blocks;
    {
        HpackEncoder encoder;
        for (int v = 0; v < kVariants; v++) {
            encoded_blocks.push_back(encoder.encode(req_headers[v]));
        }
    }

    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; i++) {
            HpackDecoder decoder;
            for (int v = 0; v < kVariants; v++) {
                auto result = decoder.decode(encoded_blocks[v]);
                if (!result) {
                    std::cerr << "Decode error!\n";
                    std::abort();
                }
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        double elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        double total_ops = static_cast<double>(iterations) * kVariants;
        result.decode_ops = total_ops / (elapsed_ns / 1e9);
        result.decode_ns = elapsed_ns / total_ops;
    }

    return result;
}

int main(int argc, char* argv[]) {
    int iterations = 200;
    if (argc > 1) iterations = std::atoi(argv[1]);

    std::cout << "========================================\n";
    std::cout << "HPACK Encode/Decode Micro-Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Iterations: " << iterations << "\n";
    std::cout << "Variants per iteration: 256\n";
    std::cout << "Total header-blocks: " << iterations * 256 << "\n";
    std::cout << "========================================\n\n";

    // Warmup
    runBench(10);

    // Real run (3 rounds, take best)
    constexpr int kRounds = 3;
    double best_encode_ops = 0, best_decode_ops = 0;
    double best_encode_ns = 1e18, best_decode_ns = 1e18;

    for (int r = 0; r < kRounds; r++) {
        auto res = runBench(iterations);
        std::cout << "Round " << (r + 1) << ": "
                  << "encode=" << static_cast<int>(res.encode_ops) << " ops/s ("
                  << static_cast<int>(res.encode_ns) << " ns/op)  "
                  << "decode=" << static_cast<int>(res.decode_ops) << " ops/s ("
                  << static_cast<int>(res.decode_ns) << " ns/op)  "
                  << "encoded_bytes=" << res.total_encoded_bytes << "\n";

        if (res.encode_ops > best_encode_ops) best_encode_ops = res.encode_ops;
        if (res.decode_ops > best_decode_ops) best_decode_ops = res.decode_ops;
        if (res.encode_ns < best_encode_ns) best_encode_ns = res.encode_ns;
        if (res.decode_ns < best_decode_ns) best_decode_ns = res.decode_ns;
    }

    std::cout << "\n========================================\n";
    std::cout << "Best of " << kRounds << " rounds:\n";
    std::cout << "  Encode: " << static_cast<int>(best_encode_ops) << " ops/s  ("
              << static_cast<int>(best_encode_ns) << " ns/op)\n";
    std::cout << "  Decode: " << static_cast<int>(best_decode_ops) << " ops/s  ("
              << static_cast<int>(best_decode_ns) << " ns/op)\n";
    std::cout << "========================================\n";

    return 0;
}
