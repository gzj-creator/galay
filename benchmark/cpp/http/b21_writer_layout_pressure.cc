/**
 * @file b21_writer_layout_pressure.cc
 * @brief HTTP writer 发送布局准备压力基准。
 *
 * 使用方法:
 *   ./benchmark_http_writer_layout_pressure [iterations]
 */

#include "../common/benchmark_environment.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#define private public
#include <galay/cpp/galay-http/kernel/http_writer.h>
#undef private

#include <galay/cpp/galay-kernel/async/async_tcp.h>

using namespace galay::http;
using namespace galay::async;

namespace {

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[benchmark_http_writer_layout_pressure] " << message << "\n";
        return false;
    }
    return true;
}

template <typename Func>
bool run_bench(const char* name, size_t iterations, Func&& func)
{
    size_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        checksum += func(i);
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    if (!require(checksum != 0, "checksum should not be zero")) {
        return false;
    }
    std::cout << name << ": " << iterations << " iterations, "
              << (static_cast<double>(iterations) / seconds)
              << " ops/s, checksum=" << checksum << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    size_t iterations = 1000000;
    if (argc > 1) {
        const long requested = std::strtol(argv[1], nullptr, 10);
        if (requested > 0) {
            iterations = static_cast<size_t>(requested);
        }
    }

    AsyncTcpSocket socket(IPType::IPV4);
    HttpWriterImpl<AsyncTcpSocket> writer(HttpWriterSetting(), socket);
    const std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 32\r\n"
        "\r\n";
    const std::string body = "0123456789abcdef0123456789abcdef";
    writer.m_buffer.reserve(header.size());
    writer.m_body_buffer.reserve(body.size());

    if (!run_bench("BM_HttpWriterTcpLayout", iterations, [&](size_t i) {
            writer.m_buffer = header;
            writer.m_body_buffer = body;
            writer.prepare_tcp_send_layout();
            const size_t count = writer.get_iovecs_count();
            const size_t remaining = writer.get_remaining_bytes();
            writer.update_remaining_writev(remaining);
            return count + remaining + (i & 1U);
        })) {
        return 1;
    }

    if (!run_bench("BM_HttpWriterSslCoalesceLayout", iterations, [&](size_t i) {
            writer.prepare_ssl_send_layout(header, body);
            const size_t remaining = writer.get_remaining_bytes();
            writer.update_remaining(remaining);
            return remaining + (i & 1U);
        })) {
        return 1;
    }

    // These cases measure synchronous layout preparation without starting socket sends.
    HttpResponse lvalue_response;
    lvalue_response.set_body_str(std::string(body));
    if (!run_bench("BM_HttpWriterLvalueResponseLayout", iterations, [&](size_t i) {
            (void) writer.send_response(lvalue_response);
            const size_t count = writer.get_iovecs_count();
            const size_t remaining = writer.get_remaining_bytes();
            writer.update_remaining_writev(remaining);
            return count + remaining + lvalue_response.body_str().size() + (i & 1U);
        })) {
        return 1;
    }

    if (!run_bench("BM_HttpWriterRvalueResponseLayout", iterations, [&](size_t i) {
            HttpResponse response;
            response.set_body_str(std::string(body));
            (void) writer.send_response(std::move(response));
            const size_t count = writer.get_iovecs_count();
            const size_t remaining = writer.get_remaining_bytes();
            writer.update_remaining_writev(remaining);
            return count + remaining + (i & 1U);
        })) {
        return 1;
    }

    if (!run_bench("BM_HttpWriterRvalueRequestLayout", iterations, [&](size_t i) {
            HttpRequest request;
            request.set_body_str(std::string(body));
            (void) writer.send_request(std::move(request));
            const size_t count = writer.get_iovecs_count();
            const size_t remaining = writer.get_remaining_bytes();
            writer.update_remaining_writev(remaining);
            return count + remaining + (i & 1U);
        })) {
        return 1;
    }

    HttpResponseHeader response_header;
    HttpRequestHeader request_header;
    if (!run_bench("BM_HttpWriterLvalueHeaderLayout", iterations, [&](size_t i) {
            (void) writer.send_header(response_header);
            const size_t response_size = writer.get_remaining_bytes();
            writer.update_remaining(response_size);

            (void) writer.send_header(request_header);
            const size_t request_size = writer.get_remaining_bytes();
            writer.update_remaining(request_size);
            return response_size + request_size + (i & 1U);
        })) {
        return 1;
    }

    return 0;
}
