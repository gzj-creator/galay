#include <galay/cpp/galay-postgres/protoc/postgres_auth.h>
#include <galay/cpp/galay-postgres/protoc/postgres_protocol.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace galay::postgres::protocol;

namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testMd5PasswordVector()
{
    constexpr std::array<uint8_t, 4> salt{1, 2, 3, 4};
    constexpr std::string_view expected = "md598a0412b9c31436fc53776e863350083";
    require(md5Password("alice", "secret", salt) == expected,
            "PostgreSQL MD5 password vector mismatch");
}

void testMd5PasswordShape()
{
    constexpr std::array<uint8_t, 4> binary_salt{0, 0x80, 0xff, 0x01};
    const std::string result = md5Password("", "", binary_salt);
    require(result.size() == 35 && result.starts_with("md5"),
            "MD5 password response must contain the md5 prefix and digest");
    for (const char value : std::string_view(result).substr(3)) {
        require((value >= '0' && value <= '9') ||
                    (value >= 'a' && value <= 'f'),
                "MD5 password response must use lowercase hexadecimal");
    }
}

void testAuthenticationRequestBoundaries()
{
    PostgresParser parser;

    std::string md5_request;
    writeInt32(md5_request, static_cast<uint32_t>(AuthRequestKind::Md5Password));
    md5_request.append("\0\x80\xff\x01", 4);
    auto parsed = parser.parseAuthenticationRequest(md5_request.data(), md5_request.size());
    require(parsed && parsed->kind == AuthRequestKind::Md5Password &&
                parsed->data == std::string("\0\x80\xff\x01", 4),
            "MD5 authentication request must preserve all four salt bytes");

    md5_request.pop_back();
    parsed = parser.parseAuthenticationRequest(md5_request.data(), md5_request.size());
    require(!parsed && parsed.error() == ParseError::InvalidLength,
            "MD5 authentication request requires exactly four salt bytes");

    std::string cleartext_request;
    writeInt32(cleartext_request,
               static_cast<uint32_t>(AuthRequestKind::CleartextPassword));
    parsed = parser.parseAuthenticationRequest(cleartext_request.data(),
                                               cleartext_request.size());
    require(parsed && parsed->kind == AuthRequestKind::CleartextPassword &&
                parsed->data.empty(),
            "cleartext authentication request parse mismatch");
    cleartext_request.push_back('\0');
    parsed = parser.parseAuthenticationRequest(cleartext_request.data(),
                                               cleartext_request.size());
    require(!parsed && parsed.error() == ParseError::InvalidLength,
            "cleartext authentication request must not contain extra bytes");
}

void testPasswordMessageEncoding()
{
    PostgresEncoder encoder;
    PostgresParser parser;

    const std::string message = encoder.encodePasswordMessage("secret");
    auto extracted = parser.extractMessage(message.data(), message.size());
    require(extracted && extracted->type == kMsgPassword &&
                std::string_view(extracted->payload, extracted->payload_len) ==
                    std::string_view("secret\0", 7),
            "cleartext and MD5 password responses must use a terminated PasswordMessage");
    require(encoder.encodePasswordMessage(std::string_view("bad\0password", 12)).empty(),
            "PasswordMessage must reject embedded NUL bytes");
}

} // namespace

int main()
{
    testMd5PasswordVector();
    testMd5PasswordShape();
    testAuthenticationRequestBoundaries();
    testPasswordMessageEncoding();
    return EXIT_SUCCESS;
}
