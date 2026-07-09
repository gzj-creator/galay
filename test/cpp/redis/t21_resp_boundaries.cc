#include <galay/cpp/galay-redis/protoc/redis_protocol.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
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

std::optional<std::string> readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        std::cerr << "failed to open " << path << "\n";
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::filesystem::path repoRoot()
{
    std::filesystem::path file = __FILE__;
    return file.parent_path().parent_path().parent_path().parent_path();
}

std::optional<std::string> bodyAfter(const std::string& text, const std::string& signature)
{
    const auto signature_pos = text.find(signature);
    if (signature_pos == std::string::npos) {
        std::cerr << "missing source boundary signature: " << signature << "\n";
        return std::nullopt;
    }

    const auto open_pos = text.find('{', signature_pos);
    if (open_pos == std::string::npos) {
        std::cerr << "missing source boundary body: " << signature << "\n";
        return std::nullopt;
    }

    size_t depth = 0;
    for (size_t i = open_pos; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0) {
                return text.substr(open_pos, i - open_pos + 1);
            }
        }
    }

    std::cerr << "unterminated source boundary body: " << signature << "\n";
    return std::nullopt;
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
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

bool testDoubleParseBoundaries()
{
    RespParser parser;
    RedisReply reply;
    const std::string input = ",1.25\r\n";
    auto parsed = parser.parseFast(input.data(), input.size(), &reply);
    if (!parsed) {
        std::cerr << "double parse failed with " << static_cast<int>(parsed.error()) << "\n";
        return false;
    }
    if (reply.getType() != RespType::Double || reply.asDouble() != 1.25) {
        std::cerr << "double parse returned wrong value\n";
        return false;
    }
    if (!expectFastParseError(",1.2x\r\n", ParseError::InvalidFormat, "double trailing garbage")) {
        return false;
    }
    if (!expectFastParseError(",1e999999\r\n", ParseError::InvalidFormat, "double out of range")) {
        return false;
    }

    const auto protocol_source = readFile(repoRoot() / "src/cpp/galay-redis/protoc/redis_protocol.cc");
    if (!protocol_source) {
        return false;
    }
    const auto double_body = bodyAfter(*protocol_source, "RespParser::parseDoubleFast");
    if (!double_body) {
        return false;
    }
    for (const auto* forbidden : {"std::stod", "try", "catch", "std::string str"}) {
        if (contains(*double_body, forbidden)) {
            std::cerr << "RESP double parser hot path must use explicit parse errors, not "
                      << forbidden << "\n";
            return false;
        }
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
    if (!testDoubleParseBoundaries()) {
        return 1;
    }
    std::cout << "T21-RedisRespBoundaries PASS\n";
    return 0;
}
