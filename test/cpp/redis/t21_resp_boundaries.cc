#include <galay/cpp/galay-redis/protoc/redis_protocol.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

using namespace galay::redis::protocol;

namespace
{

bool expectParseError(std::string_view input, ParseError expected, const char* label)
{
    RespParser parser;
    auto parsed = parser.parse(input.data(), input.size());
    if (parsed) {
        std::cerr << label << " parsed unexpectedly\n";
        return false;
    }
    if (parsed.error() != expected) {
        std::cerr << label << " expected error " << static_cast<int>(expected)
                  << " got " << static_cast<int>(parsed.error()) << "\n";
        return false;
    }
    return true;
}

bool expectFastParseError(std::string_view input, ParseError expected, const char* label)
{
    RespParser parser;
    RedisReply reply;
    auto parsed = parser.parseFast(input.data(), input.size(), &reply);
    if (parsed) {
        std::cerr << label << " parsed unexpectedly\n";
        return false;
    }
    if (parsed.error() != expected) {
        std::cerr << label << " expected error " << static_cast<int>(expected)
                  << " got " << static_cast<int>(parsed.error()) << "\n";
        return false;
    }
    return true;
}

bool expectOwnedStringReply(std::string input,
                            RespType expected_type,
                            const std::string& expected,
                            const char* label)
{
    RespParser parser;
    RedisReply reply;
    auto parsed = parser.parseFast(input.data(), input.size(), &reply);
    if (!parsed) {
        std::cerr << label << " parse failed with " << static_cast<int>(parsed.error()) << "\n";
        return false;
    }
    if (parsed.value() != input.size()) {
        std::cerr << label << " consumed " << parsed.value() << " of " << input.size() << "\n";
        return false;
    }
    if (reply.getType() != expected_type) {
        std::cerr << label << " returned unexpected type\n";
        return false;
    }

    std::fill(input.begin(), input.end(), '?');
    const std::string value = reply.asString();
    if (value != expected) {
        std::cerr << label << " did not preserve owned string payload\n";
        return false;
    }
    return true;
}

bool testStringReplyOwnership()
{
    const std::string simple_payload(64, 's');
    const std::string simple_input = std::string("+") + simple_payload + "\r\n";
    if (!expectOwnedStringReply(simple_input,
                                RespType::SimpleString,
                                simple_payload,
                                "simple string ownership")) {
        return false;
    }

    const char bulk_raw[] = {
        '$', '6', '\r', '\n', 'f', 'o', '\0', 'b', 'a', 'r', '\r', '\n'
    };
    const std::string bulk_input(bulk_raw, sizeof(bulk_raw));
    const std::string bulk_payload(bulk_raw + 4, 6);
    if (!expectOwnedStringReply(bulk_input,
                                RespType::BulkString,
                                bulk_payload,
                                "bulk string ownership")) {
        return false;
    }

    return true;
}

bool testStringParseErrors()
{
    if (!expectFastParseError("+QUEUED", ParseError::Incomplete, "simple string missing crlf")) {
        return false;
    }
    if (!expectFastParseError("$3\r\nab\r\n", ParseError::Incomplete, "bulk string truncated payload")) {
        return false;
    }
    if (!expectFastParseError("$3\r\nabcxx", ParseError::InvalidFormat, "bulk string invalid trailer")) {
        return false;
    }
    if (!expectFastParseError("$-2\r\n", ParseError::InvalidLength, "bulk string invalid negative length")) {
        return false;
    }

    RespParser parser;
    auto parsed = parser.parseFast("+OK\r\n", 5, nullptr);
    if (parsed) {
        std::cerr << "parseFast null output parsed unexpectedly\n";
        return false;
    }
    if (parsed.error() != ParseError::InvalidFormat) {
        std::cerr << "parseFast null output returned unexpected error\n";
        return false;
    }

    return true;
}

bool testBulkLengthBoundaries()
{
    if (!expectParseError("$5\r\nabc\r\n", ParseError::Incomplete, "bulk len greater than remaining")) {
        return false;
    }
    if (!expectParseError("$9223372036854775807\r\nx\r\n",
                          ParseError::InvalidLength,
                          "bulk len far above upper bound")) {
        return false;
    }
    if (!expectParseError("$536870913\r\n", ParseError::InvalidLength, "bulk len upper bound")) {
        return false;
    }
    return true;
}

bool testAggregateLengthBoundaries()
{
    if (!expectParseError("*536870913\r\n", ParseError::InvalidLength, "array len upper bound")) {
        return false;
    }
    if (!expectParseError("%268435457\r\n", ParseError::InvalidLength, "map len upper bound")) {
        return false;
    }
    if (!expectParseError("~536870913\r\n", ParseError::InvalidLength, "set len upper bound")) {
        return false;
    }
    if (!expectParseError("*2\r\n:1\r\n", ParseError::Incomplete, "array len greater than remaining")) {
        return false;
    }
    if (!expectParseError("%1\r\n+key\r\n", ParseError::Incomplete, "map value missing")) {
        return false;
    }
    if (!expectParseError("~2\r\n+a\r\n", ParseError::Incomplete, "set len greater than remaining")) {
        return false;
    }
    return true;
}

} // namespace

int main()
{
    if (!testStringReplyOwnership()) {
        return 1;
    }
    if (!testStringParseErrors()) {
        return 1;
    }
    if (!testBulkLengthBoundaries()) {
        return 1;
    }
    if (!testAggregateLengthBoundaries()) {
        return 1;
    }
    std::cout << "T21-RedisRespBoundaries PASS\n";
    return 0;
}
