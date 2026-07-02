#include "crc32c.h"

#include <array>

namespace galay::mongo::protocol::detail
{

namespace
{

constexpr uint32_t kInitialCrc32c = 0xFFFFFFFFu;
constexpr uint32_t kCastagnoliReflectedPolynomial = 0x82F63B78u;

constexpr std::array<uint32_t, 256> makeCrc32cTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (kCastagnoliReflectedPolynomial & mask);
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto kCrc32cTable = makeCrc32cTable();

} // namespace

uint32_t crc32c(const char* data, size_t len)
{
    uint32_t crc = kInitialCrc32c;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        const uint32_t table_index = (crc ^ bytes[i]) & 0xFFu;
        crc = (crc >> 8) ^ kCrc32cTable[table_index];
    }
    return ~crc;
}

} // namespace galay::mongo::protocol::detail
