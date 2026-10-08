/**
 * @file T32-H2Hpack.cc
 * @brief HPACK contract test: round-trip + table-size update + header-list limit
 */

#include <galay/cpp/galay-http2/protoc/http2_hpack.h>
#include <cassert>
#include <iostream>

using namespace galay::http2;

void test_dynamic_table_growth()
{
    HpackDynamicTable table;
    for (std::size_t count = 1; count <= 40; ++count) {
        table.add({"x-sequence", std::to_string(count)});
        assert(table.count() == count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto* field = table.get(index);
            assert(field && field->name == "x-sequence");
            assert(field->value == std::to_string(count - index));
            const auto found = table.find(field->name, field->value);
            assert(found.first == index && !found.second);
        }
    }

    table.set_max_size(256);
    for (std::size_t count = 41; count <= 80; ++count) {
        table.add({"x-sequence", std::to_string(count)});
        assert(table.current_size() <= table.max_size());
        for (std::size_t index = 0; index < table.count(); ++index) {
            const auto* field = table.get(index);
            assert(field && field->value == std::to_string(count - index));
        }
    }

    HpackEncoder encoder;
    HpackDecoder decoder;
    for (int count = 1; count <= 40; ++count) {
        const Http2HeaderField field{"x-sequence", std::to_string(count)};
        const auto decoded = decoder.decode(encoder.encode({field}));
        assert(decoded && *decoded == std::vector<Http2HeaderField>{field});
        // RFC 7541 index 62 is always the newest entry, independently of growth.
        assert(encoder.encode({field}) == std::string(1, static_cast<char>(0xbe)));
        const auto newest = decoder.decode(std::string(1, static_cast<char>(0xbe)));
        assert(newest && *newest == std::vector<Http2HeaderField>{field});
    }
}

int main() {
    test_dynamic_table_growth();
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

    HpackDecoder target_decoder;
    auto target = target_decoder.decode_request_target(block);
    assert(target.has_value());
    assert(target->method == "GET");
    assert(target->path == "/");

    auto conditional_block = encoder.encode_stateless({
        {":method", "GET"},
        {":path", "/files/small.txt"},
        {"if-none-match", "\"etag-1\""},
        {"range", "bytes=0-99"},
    });
    HpackDecoder conditional_decoder;
    auto conditional_target = conditional_decoder.decode_request_target(conditional_block);
    assert(conditional_target.has_value());
    assert(conditional_target->method == "GET");
    assert(conditional_target->path == "/files/small.txt");
    assert(conditional_target->if_none_match == "\"etag-1\"");
    assert(conditional_target->range == "bytes=0-99");

    // 2) Dynamic table size update contract
    encoder.set_max_table_size(128);
    std::vector<Http2HeaderField> headers2 = {
        {"x-custom", "value"}
    };
    auto block2 = encoder.encode(headers2);
    auto decoded2 = decoder.decode(block2);
    assert(decoded2.has_value());
    assert(decoder.dynamic_table().max_size() == 128);
    auto target2 = target_decoder.decode_request_target(block2);
    assert(target2.has_value());
    assert(target_decoder.dynamic_table().max_size() == 128);

    // 3) Header-list-size limit contract
    HpackDecoder limited_decoder;
    limited_decoder.set_max_header_list_size(48);
    auto too_large = encoder.encode({
        {"x-long-header-name", "123456789012345678901234567890"}
    });
    auto limited_result = limited_decoder.decode(too_large);
    assert(!limited_result.has_value());
    assert(limited_result.error() == Http2ErrorCode::CompressionError);

    std::cout << "T32-H2Hpack PASS\n";
    return 0;
}
