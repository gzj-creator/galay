#include <cstdint>
#include <iostream>
#include <string>

#include "galay-mongo/protoc/builder.h"
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

bool testBuilderPipelineEncode()
{
    std::cout << "Testing MongoCommandBuilder pipeline encode..." << std::endl;

    MongoCommandBuilder builder;
    builder.reserve(2);

    builder.append("ping", int32_t(1));

    MongoDocument build_info_args;
    build_info_args.append("comment", "galay");
    builder.append("buildInfo", int32_t(1), std::move(build_info_args));

    if (builder.size() != 2) {
        return failCase("builder size mismatch");
    }

    const int32_t first_request_id = 100;
    const std::string encoded = builder.encodePipeline("admin", first_request_id);

    if (encoded.empty()) {
        return failCase("encoded pipeline is empty");
    }

    size_t consumed = 0;
    auto first_msg = MongoProtocol::extractMessage(encoded.data(), encoded.size(), consumed);
    if (!first_msg) {
        return failCase("decode first message failed: " + first_msg.error().message());
    }
    if (first_msg->header.request_id != first_request_id) {
        return failCase("first request_id mismatch");
    }
    if (first_msg->body.getInt32("ping") != 1) {
        return failCase("first command mismatch");
    }
    if (first_msg->body.getString("$db") != "admin") {
        return failCase("first command missing $db");
    }

    size_t consumed2 = 0;
    const char* second_data = encoded.data() + consumed;
    const size_t second_len = encoded.size() - consumed;
    auto second_msg = MongoProtocol::extractMessage(second_data, second_len, consumed2);
    if (!second_msg) {
        return failCase("decode second message failed: " + second_msg.error().message());
    }
    if (second_msg->header.request_id != first_request_id + 1) {
        return failCase("second request_id mismatch");
    }
    if (second_msg->body.getInt32("buildInfo") != 1) {
        return failCase("second command mismatch");
    }
    if (second_msg->body.getString("$db") != "admin") {
        return failCase("second command missing $db");
    }

    if (consumed + consumed2 != encoded.size()) {
        return failCase("encoded payload has trailing bytes");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

bool testAppendOpMsgWithDatabase()
{
    std::cout << "Testing MongoProtocol::appendOpMsgWithDatabase..." << std::endl;

    MongoDocument ping_without_db;
    ping_without_db.append("ping", int32_t(1));

    std::string encoded;
    MongoProtocol::appendOpMsgWithDatabase(encoded, 101, ping_without_db, "admin");

    size_t consumed = 0;
    auto parsed = MongoProtocol::extractMessage(encoded.data(), encoded.size(), consumed);
    if (!parsed) {
        return failCase("decode appendOpMsgWithDatabase payload failed: " +
                        parsed.error().message());
    }
    if (parsed->header.request_id != 101) {
        return failCase("appendOpMsgWithDatabase request_id mismatch");
    }
    if (parsed->body.getInt32("ping") != 1) {
        return failCase("appendOpMsgWithDatabase ping mismatch");
    }
    if (parsed->body.getString("$db") != "admin") {
        return failCase("appendOpMsgWithDatabase missing injected $db");
    }

    MongoDocument ping_with_db;
    ping_with_db.append("ping", int32_t(1));
    ping_with_db.append("$db", "custom_db");
    encoded.clear();
    MongoProtocol::appendOpMsgWithDatabase(encoded, 102, ping_with_db, "admin");

    consumed = 0;
    parsed = MongoProtocol::extractMessage(encoded.data(), encoded.size(), consumed);
    if (!parsed) {
        return failCase("decode appendOpMsgWithDatabase(with $db) payload failed: " +
                        parsed.error().message());
    }
    if (parsed->body.getString("$db") != "custom_db") {
        return failCase("appendOpMsgWithDatabase should keep existing $db");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

} // namespace

int main()
{
    std::cout << "=== T8: Mongo Protocol Builder Tests ===" << std::endl;
    if (!testBuilderPipelineEncode()) {
        return 1;
    }
    if (!testAppendOpMsgWithDatabase()) {
        return 1;
    }
    std::cout << "\nAll protocol builder tests PASSED!" << std::endl;
    return 0;
}
