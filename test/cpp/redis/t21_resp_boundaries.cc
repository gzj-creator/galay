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

bool expect_parse_error(std::string_view input, ParseError expected, const char* label)
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

bool expect_fast_parse_error(std::string_view input, ParseError expected, const char* label)
{
    RespParser parser;
    RedisReply reply;
    auto parsed = parser.parse_fast(input.data(), input.size(), &reply);
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

bool expect_owned_string_reply(std::string input,
                            RespType expected_type,
                            const std::string& expected,
                            const char* label)
{
    RespParser parser;
    RedisReply reply;
    auto parsed = parser.parse_fast(input.data(), input.size(), &reply);
    if (!parsed) {
        std::cerr << label << " parse failed with " << static_cast<int>(parsed.error()) << "\n";
        return false;
    }
    if (parsed.value() != input.size()) {
        std::cerr << label << " consumed " << parsed.value() << " of " << input.size() << "\n";
        return false;
    }
    if (reply.get_type() != expected_type) {
        std::cerr << label << " returned unexpected type\n";
        return false;
    }

    std::fill(input.begin(), input.end(), '?');
    const std::string value = reply.as_string();
    if (value != expected) {
        std::cerr << label << " did not preserve owned string payload\n";
        return false;
    }
    return true;
}

std::optional<std::string> read_file(const std::filesystem::path& path)
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

std::filesystem::path repo_root()
{
    std::filesystem::path file = __FILE__;
    return file.parent_path().parent_path().parent_path().parent_path();
}

std::optional<std::string> body_after(const std::string& text, const std::string& signature)
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

bool test_string_reply_ownership()
{
    const std::string simple_payload(64, 's');
    const std::string simple_input = std::string("+") + simple_payload + "\r\n";
    if (!expect_owned_string_reply(simple_input,
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
    if (!expect_owned_string_reply(bulk_input,
                                RespType::BulkString,
                                bulk_payload,
                                "bulk string ownership")) {
        return false;
    }

    return true;
}

bool test_string_parse_errors()
{
    if (!expect_fast_parse_error("+QUEUED", ParseError::Incomplete, "simple string missing crlf")) {
        return false;
    }
    if (!expect_fast_parse_error("$3\r\nab\r\n", ParseError::Incomplete, "bulk string truncated payload")) {
        return false;
    }
    if (!expect_fast_parse_error("$3\r\nabcxx", ParseError::InvalidFormat, "bulk string invalid trailer")) {
        return false;
    }
    if (!expect_fast_parse_error("$-2\r\n", ParseError::InvalidLength, "bulk string invalid negative length")) {
        return false;
    }

    RespParser parser;
    auto parsed = parser.parse_fast("+OK\r\n", 5, nullptr);
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

bool test_bulk_length_boundaries()
{
    if (!expect_parse_error("$5\r\nabc\r\n", ParseError::Incomplete, "bulk len greater than remaining")) {
        return false;
    }
    if (!expect_parse_error("$9223372036854775807\r\nx\r\n",
                          ParseError::InvalidLength,
                          "bulk len far above upper bound")) {
        return false;
    }
    if (!expect_parse_error("$536870913\r\n", ParseError::InvalidLength, "bulk len upper bound")) {
        return false;
    }
    return true;
}

bool test_aggregate_length_boundaries()
{
    if (!expect_parse_error("*536870913\r\n", ParseError::InvalidLength, "array len upper bound")) {
        return false;
    }
    if (!expect_parse_error("%268435457\r\n", ParseError::InvalidLength, "map len upper bound")) {
        return false;
    }
    if (!expect_parse_error("~536870913\r\n", ParseError::InvalidLength, "set len upper bound")) {
        return false;
    }
    if (!expect_parse_error("*2\r\n:1\r\n", ParseError::Incomplete, "array len greater than remaining")) {
        return false;
    }
    if (!expect_parse_error("%1\r\n+key\r\n", ParseError::Incomplete, "map value missing")) {
        return false;
    }
    if (!expect_parse_error("~2\r\n+a\r\n", ParseError::Incomplete, "set len greater than remaining")) {
        return false;
    }
    return true;
}

bool test_double_parse_boundaries()
{
    RespParser parser;
    RedisReply reply;
    const std::string input = ",1.25\r\n";
    auto parsed = parser.parse_fast(input.data(), input.size(), &reply);
    if (!parsed) {
        std::cerr << "double parse failed with " << static_cast<int>(parsed.error()) << "\n";
        return false;
    }
    if (reply.get_type() != RespType::Double || reply.as_double() != 1.25) {
        std::cerr << "double parse returned wrong value\n";
        return false;
    }
    if (!expect_fast_parse_error(",1.2x\r\n", ParseError::InvalidFormat, "double trailing garbage")) {
        return false;
    }
    if (!expect_fast_parse_error(",1e999999\r\n", ParseError::InvalidFormat, "double out of range")) {
        return false;
    }

    const auto protocol_source = read_file(repo_root() / "src/cpp/galay-redis/protoc/redis_protocol.cc");
    if (!protocol_source) {
        return false;
    }
    const auto double_body = body_after(*protocol_source, "RespParser::parse_double_fast");
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
    if (!test_string_reply_ownership()) {
        return 1;
    }
    if (!test_string_parse_errors()) {
        return 1;
    }
    if (!test_bulk_length_boundaries()) {
        return 1;
    }
    if (!test_aggregate_length_boundaries()) {
        return 1;
    }
    if (!test_double_parse_boundaries()) {
        return 1;
    }
    std::cout << "T21-RedisRespBoundaries PASS\n";
    return 0;
}
