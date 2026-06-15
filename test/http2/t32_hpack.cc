/**
 * @file T32-H2Hpack.cc
 * @brief HPACK contract test: round-trip + table-size update + header-list limit
 */

#include "galay-http2/protoc/http2_hpack.h"
#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    // 1) Round-trip contract
    HpackEncoder encoder;
    HpackDecoder decoder;

    std::vector<Http2HeaderField> headers = {
        {":method", "GET"},
        {":path", "/"},
        {"user-agent", "galay-test"},
        {"accept", "*/*"},
    };

    auto block = encoder.encode(headers);
    auto decoded = decoder.decode(block);
    assert(decoded.has_value());
    assert(decoded.value() == headers);

    // 2) Dynamic table size update contract
    encoder.setMaxTableSize(128);
    std::vector<Http2HeaderField> headers2 = {
        {"x-custom", "value"}
    };
    auto block2 = encoder.encode(headers2);
    auto decoded2 = decoder.decode(block2);
    assert(decoded2.has_value());
    assert(decoder.dynamicTable().maxSize() == 128);

    // 3) Header-list-size limit contract
    HpackDecoder limited_decoder;
    limited_decoder.setMaxHeaderListSize(48);
    auto too_large = encoder.encode({
        {"x-long-header-name", "123456789012345678901234567890"}
    });
    auto limited_result = limited_decoder.decode(too_large);
    assert(!limited_result.has_value());
    assert(limited_result.error() == Http2ErrorCode::CompressionError);

    std::cout << "T32-H2Hpack PASS\n";
    return 0;
}
