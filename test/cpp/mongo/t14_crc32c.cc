#include <galay/cpp/galay-mongo/protoc/crc32c.h>

#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{

bool expectCrc32c(const std::string_view name,
                  const std::string_view input,
                  const uint32_t expected)
{
    const uint32_t actual = galay::mongo::protocol::detail::crc32c(input.data(),
                                                                   input.size());
    if (actual != expected) {
        std::cerr << "  FAILED: " << name << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
    std::cout << "=== T14: Mongo CRC32C Tests ===\n";

    if (!expectCrc32c("empty", "", 0x00000000u)) {
        return 1;
    }
    if (!expectCrc32c("standard-check", "123456789", 0xE3069283u)) {
        return 1;
    }
    if (!expectCrc32c("quick-brown-fox", "The quick brown fox jumps over the lazy dog",
                      0x22620404u)) {
        return 1;
    }

    std::cout << "  PASSED\n";
    return 0;
}
