#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/server/http_range.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/uio.h>

using namespace galay::http;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[benchmark_http_protocol_boundaries] " << message << "\n";
        std::abort();
    }
}

std::pair<HttpErrorCode, ssize_t> parseRequest(std::string& raw)
{
    iovec iov{
        .iov_base = raw.data(),
        .iov_len = raw.size(),
    };
    HttpRequest request;
    return request.fromIOVec({iov});
}

template <typename Func>
void runBench(const char* name, size_t iterations, Func&& func)
{
    const auto start = std::chrono::steady_clock::now();
    size_t accepted = 0;
    for (size_t i = 0; i < iterations; ++i) {
        accepted += func() ? 1 : 0;
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    std::cout << name << ": " << iterations << " iterations, "
              << (static_cast<double>(iterations) / seconds) << " ops/s, accepted="
              << accepted << "\n";
}

} // namespace

int main()
{
    constexpr size_t kIterations = 50000;

    std::string te_cl =
        "POST /upload HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "0\r\n\r\n";
    std::string bad_uri = "GET /bad% HTTP/1.1\r\nHost: example.com\r\n\r\n";
    std::string range_header = "bytes=0-9,5-14,15-19";

    require(parseRequest(te_cl).first == kBadRequest, "TE+CL fixture should be rejected");
    require(parseRequest(bad_uri).first == kUriEncodeError, "bad URI fixture should be rejected");
    require(HttpRangeParser::parse(range_header, 100).ranges.size() == 1,
            "range fixture should merge");

    runBench("BM_RejectTransferEncodingContentLength", kIterations, [&]() {
        return parseRequest(te_cl).first == kBadRequest;
    });
    runBench("BM_RejectTruncatedUriEscape", kIterations, [&]() {
        return parseRequest(bad_uri).first == kUriEncodeError;
    });
    runBench("BM_MergeSmallRanges", kIterations, [&]() {
        return HttpRangeParser::parse(range_header, 100).ranges.size() == 1;
    });

    return 0;
}
