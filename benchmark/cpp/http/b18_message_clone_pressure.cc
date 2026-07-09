#include <galay/cpp/galay-http/builder/http_builder.h>
#include <galay/cpp/galay-http/protoc/http_body.h>
#include <galay/cpp/galay-http/protoc/http_chunk.h>
#include <galay/cpp/galay-http/protoc/http_header.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/protoc/http_response.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/uio.h>
#include <vector>

using namespace galay::http;

namespace {

std::vector<iovec> makeIovecs(std::string& data)
{
    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = data.data();
    iovecs[0].iov_len = data.size();
    return iovecs;
}

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[benchmark_http_message_clone_pressure] " << message << "\n";
        return false;
    }
    return true;
}

HttpRequest makeRequest()
{
    return Http1_1RequestBuilder::post("/clone-pressure")
        .header("Host", "example.com")
        .header("X-Trace", "request-before")
        .body("request-body-payload")
        .buildMove();
}

HttpResponse makeResponse()
{
    return Http1_1ResponseBuilder::ok()
        .header("X-Trace", "response-before")
        .body("response-body-payload")
        .buildMove();
}

HttpRequestHeader makeRequestHeader()
{
    std::string raw =
        "POST /clone-pressure HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 20\r\n"
        "\r\n";
    HttpRequestHeader header;
    auto result = header.fromString(raw);
    if (result.first != kNoError || result.second <= 0) {
        std::cerr << "[benchmark_http_message_clone_pressure] request header fixture failed\n";
    }
    return header;
}

HttpResponseHeader makeResponseHeader()
{
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 21\r\n"
        "\r\n";
    HttpResponseHeader header;
    auto result = header.fromString(raw);
    if (result.first != kNoError || result.second <= 0) {
        std::cerr << "[benchmark_http_message_clone_pressure] response header fixture failed\n";
    }
    return header;
}

HeaderPair makeHeaderPair()
{
    HeaderPair headers(HeaderPair::Mode::ServerSide);
    if (headers.addHeaderPair("Host", "example.com") != kNoError) {
        std::cerr << "[benchmark_http_message_clone_pressure] Host fixture failed\n";
    }
    if (headers.addHeaderPair("X-Trace", "trace-value") != kNoError) {
        std::cerr << "[benchmark_http_message_clone_pressure] X-Trace fixture failed\n";
    }
    return headers;
}

PlainBody makePlainBody()
{
    PlainBody body;
    std::string payload = "plain-body-payload";
    if (!body.fromString(std::move(payload))) {
        std::cerr << "[benchmark_http_message_clone_pressure] body fixture failed\n";
    }
    return body;
}

ChunkParser makePartialChunkParser()
{
    ChunkParser parser;
    std::string first = "5\r\nHe";
    std::string output;
    auto iovecs = makeIovecs(first);
    auto result = parser.parse(iovecs, output);
    if (!result.has_value() || result->first || result->second != first.size() || output != "He") {
        std::cerr << "[benchmark_http_message_clone_pressure] chunk parser fixture failed\n";
    }
    return parser;
}

template <typename Func>
bool runBench(const char* name, size_t iterations, Func&& func)
{
    size_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        checksum += func();
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
    size_t iterations = 20000;
    if (argc > 1) {
        const long requested = std::strtol(argv[1], nullptr, 10);
        if (requested > 0) {
            iterations = static_cast<size_t>(requested);
        }
    }

    HttpRequest request = makeRequest();
    HttpResponse response = makeResponse();
    HttpRequestHeader request_header = makeRequestHeader();
    HttpResponseHeader response_header = makeResponseHeader();
    HeaderPair headers = makeHeaderPair();
    PlainBody body = makePlainBody();
    ChunkParser parser = makePartialChunkParser();

    if (!runBench("BM_HttpRequestClone", iterations, [&]() {
            HttpRequest copy = request.clone();
            return copy.bodyStr().size() + copy.header().uri().size();
        })) return 1;

    if (!runBench("BM_HttpRequestMove", iterations, [&]() {
            HttpRequest value = makeRequest();
            HttpRequest moved = std::move(value);
            return moved.bodyStr().size() + moved.header().uri().size();
        })) return 1;

    if (!runBench("BM_HttpResponseClone", iterations, [&]() {
            HttpResponse copy = response.clone();
            return copy.bodyStr().size() + static_cast<size_t>(copy.header().code());
        })) return 1;

    if (!runBench("BM_HttpResponseMove", iterations, [&]() {
            HttpResponse value = makeResponse();
            HttpResponse moved = std::move(value);
            return moved.bodyStr().size() + static_cast<size_t>(moved.header().code());
        })) return 1;

    if (!runBench("BM_RequestHeaderClone", iterations, [&]() {
            HttpRequestHeader copy = request_header.clone();
            return copy.uri().size() + copy.headerPairs().getValue("host").size();
        })) return 1;

    if (!runBench("BM_ResponseHeaderClone", iterations, [&]() {
            HttpResponseHeader copy = response_header.clone();
            return static_cast<size_t>(copy.code()) + copy.headerPairs().getValue("content-type").size();
        })) return 1;

    if (!runBench("BM_HeaderPairClone", iterations, [&]() {
            HeaderPair copy = headers.clone();
            return copy.getValue("host").size() + copy.getValue("x-trace").size();
        })) return 1;

    if (!runBench("BM_PlainBodyClone", iterations, [&]() {
            PlainBody copy = body.clone();
            return copy.toString().size();
        })) return 1;

    if (!runBench("BM_PlainBodyMove", iterations, [&]() {
            PlainBody value = makePlainBody();
            PlainBody moved = std::move(value);
            return moved.toString().size();
        })) return 1;

    if (!runBench("BM_ChunkParserClone", iterations, [&]() {
            ChunkParser copy = parser.clone();
            std::string tail = "llo\r\n0\r\n\r\n";
            std::string output = "He";
            auto iovecs = makeIovecs(tail);
            auto result = copy.parse(iovecs, output);
            return result.has_value() && result->first ? output.size() : 0;
        })) return 1;

    if (!runBench("BM_ChunkParserMove", iterations, [&]() {
            ChunkParser value = makePartialChunkParser();
            ChunkParser moved = std::move(value);
            std::string tail = "llo\r\n0\r\n\r\n";
            std::string output = "He";
            auto iovecs = makeIovecs(tail);
            auto result = moved.parse(iovecs, output);
            return result.has_value() && result->first ? output.size() : 0;
        })) return 1;

    return 0;
}
