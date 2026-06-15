#include <iostream>
#include <vector>
#include <sys/uio.h>

#include <sstream>

#define private public
#include "galay-http/protoc/http_header.h"
#undef private

int main()
{
    using namespace galay::http;

    constexpr char kRequest[] =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1:8080\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    std::vector<iovec> iovecs = {
        {
            .iov_base = const_cast<char*>(kRequest),
            .iov_len = sizeof(kRequest) - 1,
        },
    };

    HttpRequestHeader header;
    const auto result = header.fromIOVec(iovecs);
    if (result.first != kNoError || result.second != static_cast<ssize_t>(sizeof(kRequest) - 1)) {
        std::cerr << "[T72] request should parse successfully before reset\n";
        return 1;
    }
    if (!header.headerPairs().hasCommonHeader(CommonHeaderIndex::Host) ||
        !header.headerPairs().hasCommonHeader(CommonHeaderIndex::Connection)) {
        std::cerr << "[T72] parsed request should populate common header fast-path storage\n";
        return 1;
    }

    header.reset();

    if (header.headerPairs().hasCommonHeader(CommonHeaderIndex::Host) ||
        header.headerPairs().hasCommonHeader(CommonHeaderIndex::Connection)) {
        std::cerr << "[T72] reset should clear common header fast-path state\n";
        return 1;
    }
    if (header.headerPairs().hasKey("host") || header.headerPairs().hasKey("connection")) {
        std::cerr << "[T72] reset should hide common headers from lookup API\n";
        return 1;
    }
    if (header.headerPairs().getValue("host") != "" || header.headerPairs().getValue("connection") != "") {
        std::cerr << "[T72] reset should clear common header values\n";
        return 1;
    }

    std::cout << "T72-HttpRequestHeaderResetClearsCommonHeaders PASS\n";
    return 0;
}
