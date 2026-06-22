#include <galay/cpp/galay-mongo/base/mongo_value.h>
#include <galay/cpp/galay-mongo/protoc/mongo_protocol.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace galay::mongo;
using namespace galay::mongo::protocol;

namespace
{

size_t parseIterations(int argc, char** argv)
{
    if (argc < 2) {
        return 200000;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || parsed == 0) {
        return 200000;
    }
    return static_cast<size_t>(parsed);
}

template <typename Fn>
double benchNsPerOp(size_t iterations, Fn&& fn)
{
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        fn(i);
    }
    const auto finish = std::chrono::steady_clock::now();
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
    return static_cast<double>(elapsed_ns) / static_cast<double>(iterations);
}

} // namespace

int main(int argc, char** argv)
{
    const size_t iterations = parseIterations(argc, argv);

    size_t invalid_oid_unexpected_success = 0;
    const double invalid_oid_ns = benchNsPerOp(iterations, [&](size_t) {
        auto value = MongoValue::fromObjectId("not-a-24-byte-objectid");
        if (value.has_value()) {
            ++invalid_oid_unexpected_success;
        }
    });

    MongoDocument invalid_key_doc;
    invalid_key_doc.append(std::string("bad\0key", 7), int32_t(1));
    size_t invalid_key_unexpected_success = 0;
    const double invalid_key_ns = benchNsPerOp(iterations, [&](size_t) {
        auto encoded = BsonCodec::encodeDocument(invalid_key_doc);
        if (encoded.has_value()) {
            ++invalid_key_unexpected_success;
        }
    });

    MongoDocument ping;
    ping.append("ping", int32_t(1));
    ping.append("$db", "admin");
    size_t valid_op_msg_errors = 0;
    size_t total_bytes = 0;
    const double valid_op_msg_ns = benchNsPerOp(iterations, [&](size_t i) {
        auto encoded = MongoProtocol::encodeOpMsg(static_cast<int32_t>(i + 1), ping);
        if (!encoded.has_value()) {
            ++valid_op_msg_errors;
            return;
        }
        total_bytes += encoded->size();
    });

    std::cout << "B3-MongoExpectedErrorBoundaries iterations=" << iterations << '\n';
    std::cout << "invalid_object_id_ns_per_op=" << invalid_oid_ns
              << " unexpected_success=" << invalid_oid_unexpected_success << '\n';
    std::cout << "invalid_bson_key_ns_per_op=" << invalid_key_ns
              << " unexpected_success=" << invalid_key_unexpected_success << '\n';
    std::cout << "valid_op_msg_ns_per_op=" << valid_op_msg_ns
              << " errors=" << valid_op_msg_errors
              << " total_bytes=" << total_bytes << '\n';

    if (invalid_oid_unexpected_success != 0 ||
        invalid_key_unexpected_success != 0 ||
        valid_op_msg_errors != 0 ||
        total_bytes == 0) {
        return 1;
    }
    return 0;
}
