/**
 * @file b21_writer_layout_pressure.cc
 * @brief HTTP writer 发送布局准备压力基准。
 *
 * 使用方法:
 *   ./benchmark_http_writer_layout_pressure [iterations]
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#define private public
#include <galay/cpp/galay-http/kernel/http_writer.h>
#undef private

#include <galay/cpp/galay-kernel/async/tcp_socket.h>

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
bool runBench(const char* name, size_t iterations, Func&& func)
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
    size_t iterations = 1000000;
    if (argc > 1) {
        const long requested = std::strtol(argv[1], nullptr, 10);
        if (requested > 0) {
            iterations = static_cast<size_t>(requested);
        }
    }

    TcpSocket socket(IPType::IPV4);
    HttpWriterImpl<TcpSocket> writer(HttpWriterSetting(), socket);
    const std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 32\r\n"
        "\r\n";
    const std::string body = "0123456789abcdef0123456789abcdef";
    writer.m_buffer.reserve(header.size());
    writer.m_body_buffer.reserve(body.size());

    if (!runBench("BM_HttpWriterTcpLayout", iterations, [&](size_t i) {
            writer.m_buffer = header;
            writer.m_body_buffer = body;
            writer.prepareTcpSendLayout();
            const size_t count = writer.getIovecsCount();
            const size_t remaining = writer.getRemainingBytes();
            writer.updateRemainingWritev(remaining);
            return count + remaining + (i & 1U);
        })) {
        return 1;
    }

    if (!runBench("BM_HttpWriterSslCoalesceLayout", iterations, [&](size_t i) {
            writer.prepareSslSendLayout(header, body);
            const size_t remaining = writer.getRemainingBytes();
            writer.updateRemaining(remaining);
            return remaining + (i & 1U);
        })) {
        return 1;
    }

    return 0;
}
