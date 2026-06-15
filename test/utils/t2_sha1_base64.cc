#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#include <galay-utils/crypto/sha1.hpp>
#include <galay-utils/encoding/base64.hpp>

int main()
{
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string input = key + magic;

    const std::array<uint8_t, 20> digest =
        galay::utils::SHA1::hash(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    const std::string accept = galay::utils::Base64Util::Base64Encode(digest.data(), digest.size());

    if (accept != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") {
        std::cerr << "unexpected accept key: " << accept << '\n';
        return 1;
    }
    return 0;
}
