#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/kernel/http_session.h>
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

std::pair<HttpErrorCode, ssize_t> parse_request(std::string& raw)
{
    iovec iov{
        .iov_base = raw.data(),
        .iov_len = raw.size(),
    };
    HttpRequest request;
    return request.from_io_vec({iov});
}

bool session_rejects_oversized_response(std::string& raw)
{
    galay::async::AsyncTcpSocket socket;
    HttpReaderSetting setting;
    setting.set_max_body_size(4);
    HttpSession session(socket, 1024, setting);
    galay::http::detail::HttpSessionState<galay::async::AsyncTcpSocket> state(
        session, std::string("GET / HTTP/1.1\r\n\r\n"));

    require(session.get_ring_buffer().try_write_batch(raw.data(), raw.size()) ==
                raw.size(),
            "failed to seed oversized response fixture");
    state.on_bytes_received(raw.size());
    if (!state.parse_from_ring_buffer()) {
        return false;
    }

    auto result = state.take_result();
    return !result.has_value() && result.error().code() == kRequestEntityTooLarge;
}

template <typename Func>
void run_bench(const char* name, size_t iterations, Func&& func)
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
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

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
    std::string oversized_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "12345";

    require(parse_request(te_cl).first == kBadRequest, "TE+CL fixture should be rejected");
    require(parse_request(bad_uri).first == kUriEncodeError, "bad URI fixture should be rejected");
    require(HttpRangeParser::parse(range_header, 100).ranges.size() == 1,
            "range fixture should merge");
    require(session_rejects_oversized_response(oversized_response),
            "oversized session response fixture should be rejected");

    run_bench("BM_RejectTransferEncodingContentLength", kIterations, [&]() {
        return parse_request(te_cl).first == kBadRequest;
    });
    run_bench("BM_RejectTruncatedUriEscape", kIterations, [&]() {
        return parse_request(bad_uri).first == kUriEncodeError;
    });
    run_bench("BM_MergeSmallRanges", kIterations, [&]() {
        return HttpRangeParser::parse(range_header, 100).ranges.size() == 1;
    });
    run_bench("BM_RejectOversizedSessionResponse", kIterations, [&]() {
        return session_rejects_oversized_response(oversized_response);
    });

    return 0;
}
