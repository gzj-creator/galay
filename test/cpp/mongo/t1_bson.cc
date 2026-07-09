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

void writeInt32LEAt(std::string& out, size_t pos, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out[pos + 0] = static_cast<char>(u & 0xFF);
    out[pos + 1] = static_cast<char>((u >> 8) & 0xFF);
    out[pos + 2] = static_cast<char>((u >> 16) & 0xFF);
    out[pos + 3] = static_cast<char>((u >> 24) & 0xFF);
}

void appendUint32LE(std::string& out, uint32_t value)
{
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void appendChecksum(std::string& wire)
{
    writeInt32LEAt(wire, 0, static_cast<int32_t>(wire.size() + 4));
    appendUint32LE(wire, crc32c(wire));
}

bool failCase(const std::string& message)
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

    const auto encoded = BsonCodec::encodeDocument(doc);
    if (!encoded) {
        return failCase("encodeDocument failed: " + encoded.error());
    }
    auto decoded = BsonCodec::decodeDocument(encoded->data(), encoded->size());
    if (!decoded.has_value()) {
        return failCase("decodeDocument failed: " + decoded.error());
    }

    if (decoded->getString("name") != "galay") {
        return failCase("field name mismatch");
    }
    if (decoded->getInt32("age") != 18) {
        return failCase("field age mismatch");
    }
    if (!decoded->getBool("active")) {
        return failCase("field active mismatch");
    }

    const auto* profile = decoded->find("profile");
    if (profile == nullptr || !profile->isDocument()) {
        return failCase("profile missing or invalid type");
    }
    if (profile->toDocument().getString("city") != "shanghai") {
        return failCase("profile.city mismatch");
    }

    const auto* tag_values = decoded->find("tags");
    if (tag_values == nullptr || !tag_values->isArray()) {
        return failCase("tags missing or invalid type");
    }
    if (tag_values->toArray().size() != 2) {
        return failCase("tags size mismatch");
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
    auto too_short = BsonCodec::decodeDocument(too_short_length.data(),
                                               too_short_length.size(),
                                               consumed);
    if (too_short.has_value()) {
        return failCase("BSON length smaller than minimum should fail");
    }
    if (consumed != 123) {
        return failCase("failed BSON decode should not report consumed bytes");
    }

    auto invalid_oid = MongoValue::fromObjectId("not-a-24-byte-objectid");
    if (invalid_oid.has_value()) {
        return failCase("ObjectId must reject non-24-hex input through std::expected");
    }
    auto valid_oid = MongoValue::fromObjectId("0123456789abcdefABCDEF12");
    if (!valid_oid.has_value()) {
        return failCase("ObjectId must accept 24-character hex input: " + valid_oid.error());
    }
    if (!valid_oid->isObjectId() || valid_oid->toString() != "0123456789abcdefABCDEF12") {
        return failCase("ObjectId expected value mismatch");
    }

    MongoDocument invalid_key_doc;
    invalid_key_doc.append(std::string("bad\0key", 7), int32_t(1));
    auto invalid_key = BsonCodec::encodeDocument(invalid_key_doc);
    if (invalid_key.has_value()) {
        return failCase("BSON keys with embedded NUL must fail through std::expected");
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

    const auto wire = MongoProtocol::encodeOpMsg(123, command);
    if (!wire) {
        return failCase("encodeOpMsg failed: " + wire.error());
    }

    size_t consumed = 0;
    auto parsed = MongoProtocol::extractMessage(wire->data(), wire->size(), consumed);
    if (!parsed.has_value()) {
        return failCase("extractMessage failed: " + parsed.error().message());
    }
    if (consumed != wire->size()) {
        return failCase("consumed bytes mismatch");
    }
    if (parsed->header.request_id != 123) {
        return failCase("request_id mismatch");
    }
    if (parsed->header.op_code != kMongoOpMsg) {
        return failCase("op_code mismatch");
    }
    if (parsed->body.getInt32("ping") != 1) {
        return failCase("ping value mismatch");
    }
    if (parsed->body.getString("$db") != "admin") {
        return failCase("$db value mismatch");
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

    auto valid_or_err = MongoProtocol::encodeOpMsg(124, command, 0x01);
    if (!valid_or_err) {
        return failCase("encodeOpMsg(checksum) failed: " + valid_or_err.error());
    }
    auto valid = std::move(valid_or_err.value());
    appendChecksum(valid);
    auto parsed = MongoProtocol::decodeMessage(valid.data(), valid.size());
    if (!parsed) {
        return failCase("valid OP_MSG checksum should decode: " + parsed.error().message());
    }

    auto invalid = valid;
    invalid.back() = static_cast<char>(static_cast<unsigned char>(invalid.back()) ^ 0x01u);
    auto rejected = MongoProtocol::decodeMessage(invalid.data(), invalid.size());
    if (rejected) {
        return failCase("invalid OP_MSG checksum should be rejected");
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
