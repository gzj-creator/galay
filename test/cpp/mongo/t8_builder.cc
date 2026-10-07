#include <cstdint>
#include <iostream>
#include <string>

#include <galay/cpp/galay-mongo/protoc/builder.h>
#include <galay/cpp/galay-mongo/protoc/mongo_protocol.h>

using namespace galay::mongo;
using namespace galay::mongo::protocol;

namespace
{

bool fail_case(const std::string& message)
{
    std::cerr << "  FAILED: " << message << std::endl;
    return false;
}

bool test_builder_pipeline_encode()
{
    std::cout << "Testing MongoCommandBuilder pipeline encode..." << std::endl;

    MongoCommandBuilder builder;
    builder.reserve(2);

    builder.append("ping", int32_t(1));

    MongoDocument build_info_args;
    build_info_args.append("comment", "galay");
    builder.append("buildInfo", int32_t(1), std::move(build_info_args));

    if (builder.size() != 2) {
        return fail_case("builder size mismatch");
    }

    const int32_t first_request_id = 100;
    const auto encoded_or_err = builder.encode_pipeline("admin", first_request_id);
    if (!encoded_or_err) {
        return fail_case("encodePipeline failed: " + encoded_or_err.error());
    }
    const std::string& encoded = encoded_or_err.value();

    if (encoded.empty()) {
        return fail_case("encoded pipeline is empty");
    }

    size_t consumed = 0;
    auto first_msg = MongoProtocol::extract_message(encoded.data(), encoded.size(), consumed);
    if (!first_msg) {
        return fail_case("decode first message failed: " + first_msg.error().message());
    }
    if (first_msg->header.request_id != first_request_id) {
        return fail_case("first request_id mismatch");
    }
    if (first_msg->body.get_int32("ping") != 1) {
        return fail_case("first command mismatch");
    }
    if (first_msg->body.get_string("$db") != "admin") {
        return fail_case("first command missing $db");
    }

    size_t consumed2 = 0;
    const char* second_data = encoded.data() + consumed;
    const size_t second_len = encoded.size() - consumed;
    auto second_msg = MongoProtocol::extract_message(second_data, second_len, consumed2);
    if (!second_msg) {
        return fail_case("decode second message failed: " + second_msg.error().message());
    }
    if (second_msg->header.request_id != first_request_id + 1) {
        return fail_case("second request_id mismatch");
    }
    if (second_msg->body.get_int32("buildInfo") != 1) {
        return fail_case("second command mismatch");
    }
    if (second_msg->body.get_string("$db") != "admin") {
        return fail_case("second command missing $db");
    }

    if (consumed + consumed2 != encoded.size()) {
        return fail_case("encoded payload has trailing bytes");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

bool test_append_op_msg_with_database()
{
    std::cout << "Testing MongoProtocol::appendOpMsgWithDatabase..." << std::endl;

    MongoDocument ping_without_db;
    ping_without_db.append("ping", int32_t(1));

    std::string encoded;
    auto appended = MongoProtocol::append_op_msg_with_database(encoded, 101, ping_without_db, "admin");
    if (!appended) {
        return fail_case("appendOpMsgWithDatabase failed: " + appended.error());
    }

    size_t consumed = 0;
    auto parsed = MongoProtocol::extract_message(encoded.data(), encoded.size(), consumed);
    if (!parsed) {
        return fail_case("decode appendOpMsgWithDatabase payload failed: " +
                        parsed.error().message());
    }
    if (parsed->header.request_id != 101) {
        return fail_case("appendOpMsgWithDatabase request_id mismatch");
    }
    if (parsed->body.get_int32("ping") != 1) {
        return fail_case("appendOpMsgWithDatabase ping mismatch");
    }
    if (parsed->body.get_string("$db") != "admin") {
        return fail_case("appendOpMsgWithDatabase missing injected $db");
    }

    MongoDocument ping_with_db;
    ping_with_db.append("ping", int32_t(1));
    ping_with_db.append("$db", "custom_db");
    encoded.clear();
    appended = MongoProtocol::append_op_msg_with_database(encoded, 102, ping_with_db, "admin");
    if (!appended) {
        return fail_case("appendOpMsgWithDatabase(with $db) failed: " + appended.error());
    }

    consumed = 0;
    parsed = MongoProtocol::extract_message(encoded.data(), encoded.size(), consumed);
    if (!parsed) {
        return fail_case("decode appendOpMsgWithDatabase(with $db) payload failed: " +
                        parsed.error().message());
    }
    if (parsed->body.get_string("$db") != "custom_db") {
        return fail_case("appendOpMsgWithDatabase should keep existing $db");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

} // namespace

int main()
{
    std::cout << "=== T8: Mongo Protocol Builder Tests ===" << std::endl;
    if (!test_builder_pipeline_encode()) {
        return 1;
    }
    if (!test_append_op_msg_with_database()) {
        return 1;
    }
    std::cout << "\nAll protocol builder tests PASSED!" << std::endl;
    return 0;
}
