#include <iostream>
#include <string>
#include <vector>

#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/http2_stream.h>
#undef private

using namespace galay::http2;

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T74] " << message << "\n";
        return false;
    }
    return true;
}

std::string large_string(char ch, size_t size) {
    return std::string(size, ch);
}

} // namespace

int main() {
    {
        auto stream = Http2Stream::create(1);
        std::vector<Http2HeaderField> headers;
        headers.emplace_back(":method", "POST");
        headers.emplace_back(":scheme", "https");
        headers.emplace_back(":authority", "127.0.0.1:9443");
        headers.emplace_back(":path", "/echo");
        headers.emplace_back(large_string('n', 64), large_string('v', 128));
        stream->set_decoded_headers(std::move(headers));

        const auto* name_ptr = stream->decoded_headers().back().name.data();
        const auto* value_ptr = stream->decoded_headers().back().value.data();
        stream->consume_decoded_headers_as_request();

        if (!check(stream->request().headers.size() == 1, "request should keep one regular header")) {
            return 1;
        }
        if (!check(stream->request().headers[0].name.data() == name_ptr,
                   "request regular header name should reuse decoded storage")) {
            return 1;
        }
        if (!check(stream->request().headers[0].value.data() == value_ptr,
                   "request regular header value should reuse decoded storage")) {
            return 1;
        }
    }

    {
        auto stream = Http2Stream::create(3);
        std::vector<Http2HeaderField> headers;
        headers.emplace_back(":status", "200");
        headers.emplace_back(large_string('x', 64), large_string('y', 128));
        stream->set_decoded_headers(std::move(headers));

        const auto* name_ptr = stream->decoded_headers().back().name.data();
        const auto* value_ptr = stream->decoded_headers().back().value.data();
        stream->consume_decoded_headers_as_response();

        if (!check(stream->response().headers.size() == 1, "response should keep one regular header")) {
            return 1;
        }
        if (!check(stream->response().headers[0].name.data() == name_ptr,
                   "response regular header name should reuse decoded storage")) {
            return 1;
        }
        if (!check(stream->response().headers[0].value.data() == value_ptr,
                   "response regular header value should reuse decoded storage")) {
            return 1;
        }
    }

    std::cout << "T74-H2DecodedHeadersMoveFastPath PASS\n";
    return 0;
}
