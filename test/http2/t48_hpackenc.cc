/**
 * @file T48-HpackStatelessEncode.cc
 * @brief HPACK stateless/no-index encode contract
 */

#include "galay-http2/protoc/http2_hpack.h"
#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    HpackEncoder encoder;

    auto seeded = encoder.encode({
        {"x-seeded", "seed"}
    });
    assert(!seeded.empty());

    const size_t dynamic_count_before = encoder.dynamicTable().count();
    assert(dynamic_count_before > 0);

    std::vector<Http2HeaderField> headers = {
        {":status", "200"},
        {"content-type", "text/plain"},
        {"content-length", "128"},
        {"x-response-id", "abc123"},
    };

    auto block = encoder.encodeStateless(headers);

    HpackDecoder decoder;
    auto decoded = decoder.decode(block);
    assert(decoded.has_value());
    assert(decoded.value() == headers);
    assert(decoder.dynamicTable().count() == 0);
    assert(encoder.dynamicTable().count() == dynamic_count_before);

    std::cout << "T48-HpackStatelessEncode PASS\n";
    return 0;
}
