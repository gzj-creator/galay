#include <iostream>
#include <vector>

#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/http2_stream.h>
#undef private

using namespace galay::http2;

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T75] " << message << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    auto stream = Http2Stream::create(1);

    std::vector<Http2HeaderField> headers;
    headers.emplace_back(":method", "POST");
    headers.emplace_back(":scheme", "https");
    headers.emplace_back(":authority", "127.0.0.1:9443");
    headers.emplace_back(":path", "/echo");
    headers.emplace_back("accept-encoding", "gzip");
    headers.emplace_back("content-type", "text/plain");
    headers.emplace_back("content-length", "128");
    headers.emplace_back("user-agent", "Go-http-client/2.0");
    headers.emplace_back("x-extra", "keep-me");
    stream->setDecodedHeaders(std::move(headers));
    stream->consumeDecodedHeadersAsRequest();

    if (!check(stream->request().getHeader("accept-encoding") == "gzip",
               "request should expose accept-encoding through lookup")) {
        return 1;
    }
    if (!check(stream->request().getHeader("content-type") == "text/plain",
               "request should expose content-type through lookup")) {
        return 1;
    }
    if (!check(stream->request().getHeader("content-length") == "128",
               "request should expose content-length through lookup")) {
        return 1;
    }
    if (!check(stream->request().getHeader("user-agent") == "Go-http-client/2.0",
               "request should expose user-agent through lookup")) {
        return 1;
    }
    if (!check(stream->request().headers.size() == 1,
               "only uncommon headers should remain in request header vector")) {
        return 1;
    }
    if (!check(stream->request().headers[0].name == "x-extra" &&
                   stream->request().headers[0].value == "keep-me",
               "uncommon headers should still be preserved in request header vector")) {
        return 1;
    }

    stream->request().clear();
    if (!check(stream->request().getHeader("content-type").empty(),
               "request clear should reset common header fast-path state")) {
        return 1;
    }
    if (!check(stream->request().headers.empty(),
               "request clear should reset uncommon header vector")) {
        return 1;
    }

    std::cout << "T75-H2CommonHeaderFastPath PASS\n";
    return 0;
}
