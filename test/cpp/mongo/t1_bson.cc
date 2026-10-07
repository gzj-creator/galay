#include <iostream>
#include <string>

#include <galay/cpp/galay-mongo/protoc/bson.h>
#include <galay/cpp/galay-mongo/protoc/mongo_protocol.h>

using namespace galay::mongo;
using namespace galay::mongo::protocol;

namespace
{

uint32_t crc32c(std::string_view bytes)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return ~crc;
}

void write_int32_le_at(std::string& out, size_t pos, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out[pos + 0] = static_cast<char>(u & 0xFF);
    out[pos + 1] = static_cast<char>((u >> 8) & 0xFF);
    out[pos + 2] = static_cast<char>((u >> 16) & 0xFF);
    out[pos + 3] = static_cast<char>((u >> 24) & 0xFF);
}

void append_uint32_le(std::string& out, uint32_t value)
{
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void append_checksum(std::string& wire)
{
    write_int32_le_at(wire, 0, static_cast<int32_t>(wire.size() + 4));
    append_uint32_le(wire, crc32c(wire));
}

bool fail_case(const std::string& message)
{
    std::cerr << "  FAILED: " << message << std::endl;
    return false;
}

} // namespace

bool test_bson_encode_decode()
{
    std::cout << "Testing BSON encode/decode..." << std::endl;

    MongoDocument doc;
    doc.append("name", "galay");
    doc.append("age", int32_t(18));
    doc.append("score", 95.5);
    doc.append("active", true);

    MongoDocument nested;
    nested.append("city", "shanghai");
    nested.append("zip", int32_t(200000));
    doc.append("profile", std::move(nested));

    MongoArray tags;
    tags.append("cpp");
    tags.append("mongodb");
    doc.append("tags", std::move(tags));

    const auto encoded = BsonCodec::encode_document(doc);
    if (!encoded) {
        return fail_case("encodeDocument failed: " + encoded.error());
    }
    auto decoded = BsonCodec::decode_document(encoded->data(), encoded->size());
    if (!decoded.has_value()) {
        return fail_case("decodeDocument failed: " + decoded.error());
    }

    if (decoded->get_string("name") != "galay") {
        return fail_case("field name mismatch");
    }
    if (decoded->get_int32("age") != 18) {
        return fail_case("field age mismatch");
    }
    if (!decoded->get_bool("active")) {
        return fail_case("field active mismatch");
    }

    const auto* profile = decoded->find("profile");
    if (profile == nullptr || !profile->is_document()) {
        return fail_case("profile missing or invalid type");
    }
    if (profile->to_document().get_string("city") != "shanghai") {
        return fail_case("profile.city mismatch");
    }

    const auto* tag_values = decoded->find("tags");
    if (tag_values == nullptr || !tag_values->is_array()) {
        return fail_case("tags missing or invalid type");
    }
    if (tag_values->to_array().size() != 2) {
        return fail_case("tags size mismatch");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

bool test_bson_boundaries()
{
    std::cout << "Testing BSON boundaries..." << std::endl;

    std::string too_short_length;
    too_short_length.push_back('\x04');
    too_short_length.push_back('\0');
    too_short_length.push_back('\0');
    too_short_length.push_back('\0');
    size_t consumed = 123;
    auto too_short = BsonCodec::decode_document(too_short_length.data(),
                                               too_short_length.size(),
                                               consumed);
    if (too_short.has_value()) {
        return fail_case("BSON length smaller than minimum should fail");
    }
    if (consumed != 123) {
        return fail_case("failed BSON decode should not report consumed bytes");
    }

    auto invalid_oid = MongoValue::from_object_id("not-a-24-byte-objectid");
    if (invalid_oid.has_value()) {
        return fail_case("ObjectId must reject non-24-hex input through std::expected");
    }
    auto valid_oid = MongoValue::from_object_id("0123456789abcdefABCDEF12");
    if (!valid_oid.has_value()) {
        return fail_case("ObjectId must accept 24-character hex input: " + valid_oid.error());
    }
    if (!valid_oid->is_object_id() || valid_oid->to_string() != "0123456789abcdefABCDEF12") {
        return fail_case("ObjectId expected value mismatch");
    }

    MongoDocument invalid_key_doc;
    invalid_key_doc.append(std::string("bad\0key", 7), int32_t(1));
    auto invalid_key = BsonCodec::encode_document(invalid_key_doc);
    if (invalid_key.has_value()) {
        return fail_case("BSON keys with embedded NUL must fail through std::expected");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

bool test_op_msg_encode_decode()
{
    std::cout << "Testing OP_MSG encode/decode..." << std::endl;

    MongoDocument command;
    command.append("ping", int32_t(1));
    command.append("$db", "admin");

    const auto wire = MongoProtocol::encode_op_msg(123, command);
    if (!wire) {
        return fail_case("encodeOpMsg failed: " + wire.error());
    }

    size_t consumed = 0;
    auto parsed = MongoProtocol::extract_message(wire->data(), wire->size(), consumed);
    if (!parsed.has_value()) {
        return fail_case("extractMessage failed: " + parsed.error().message());
    }
    if (consumed != wire->size()) {
        return fail_case("consumed bytes mismatch");
    }
    if (parsed->header.request_id != 123) {
        return fail_case("request_id mismatch");
    }
    if (parsed->header.op_code != kMongoOpMsg) {
        return fail_case("op_code mismatch");
    }
    if (parsed->body.get_int32("ping") != 1) {
        return fail_case("ping value mismatch");
    }
    if (parsed->body.get_string("$db") != "admin") {
        return fail_case("$db value mismatch");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

bool test_op_msg_checksum()
{
    std::cout << "Testing OP_MSG checksum verification..." << std::endl;

    MongoDocument command;
    command.append("ping", int32_t(1));
    command.append("$db", "admin");

    auto valid_or_err = MongoProtocol::encode_op_msg(124, command, 0x01);
    if (!valid_or_err) {
        return fail_case("encodeOpMsg(checksum) failed: " + valid_or_err.error());
    }
    auto valid = std::move(valid_or_err.value());
    append_checksum(valid);
    auto parsed = MongoProtocol::decode_message(valid.data(), valid.size());
    if (!parsed) {
        return fail_case("valid OP_MSG checksum should decode: " + parsed.error().message());
    }

    auto invalid = valid;
    invalid.back() = static_cast<char>(static_cast<unsigned char>(invalid.back()) ^ 0x01u);
    auto rejected = MongoProtocol::decode_message(invalid.data(), invalid.size());
    if (rejected) {
        return fail_case("invalid OP_MSG checksum should be rejected");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

int main()
{
    std::cout << "=== T1: BSON & Mongo Protocol Tests ===" << std::endl;
    if (!test_bson_encode_decode()) {
        return 1;
    }
    if (!test_bson_boundaries()) {
        return 1;
    }
    if (!test_op_msg_encode_decode()) {
        return 1;
    }
    if (!test_op_msg_checksum()) {
        return 1;
    }
    std::cout << "\nAll protocol tests PASSED!" << std::endl;
    return 0;
}
