#include <galay/cpp/galay-redis/protoc/redis_protocol.h>

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
    if (!testBulkLengthBoundaries()) {
        return 1;
    }
    if (!testAggregateLengthBoundaries()) {
        return 1;
    }
    std::cout << "T21-RedisRespBoundaries PASS\n";
    return 0;
}
