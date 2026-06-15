#include <iostream>
#include <string>

#include "galay-mongo/protoc/bson.h"
#include "galay-mongo/protoc/mongo_protocol.h"

using namespace galay::mongo;
using namespace galay::mongo::protocol;

namespace
{

bool failCase(const std::string& message)
{
    std::cerr << "  FAILED: " << message << std::endl;
    return false;
}

} // namespace

bool testBsonEncodeDecode()
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
    doc.append("profile", nested);

    MongoArray tags;
    tags.append("cpp");
    tags.append("mongodb");
    doc.append("tags", tags);

    const auto encoded = BsonCodec::encodeDocument(doc);
    auto decoded = BsonCodec::decodeDocument(encoded.data(), encoded.size());
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

bool testOpMsgEncodeDecode()
{
    std::cout << "Testing OP_MSG encode/decode..." << std::endl;

    MongoDocument command;
    command.append("ping", int32_t(1));
    command.append("$db", "admin");

    const auto wire = MongoProtocol::encodeOpMsg(123, command);

    size_t consumed = 0;
    auto parsed = MongoProtocol::extractMessage(wire.data(), wire.size(), consumed);
    if (!parsed.has_value()) {
        return failCase("extractMessage failed: " + parsed.error().message());
    }
    if (consumed != wire.size()) {
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

int main()
{
    std::cout << "=== T1: BSON & Mongo Protocol Tests ===" << std::endl;
    if (!testBsonEncodeDecode()) {
        return 1;
    }
    if (!testOpMsgEncodeDecode()) {
        return 1;
    }
    std::cout << "\nAll protocol tests PASSED!" << std::endl;
    return 0;
}
