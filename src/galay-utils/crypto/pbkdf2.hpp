/**
 * @file pbkdf2.hpp
 * @brief PBKDF2-HMAC-SHA256 helper.
 */

#ifndef GALAY_UTILS_PBKDF2_HPP
#define GALAY_UTILS_PBKDF2_HPP

#include "galay-utils/crypto/hmac.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace galay::utils {

class PBKDF2 {
public:
    /**
     * @brief Derive key bytes with PBKDF2-HMAC-SHA256.
     * @param password Non-owning password bytes; valid for password_len bytes during the call.
     * @param password_len Password length in bytes.
     * @param salt Non-owning salt bytes; valid for salt_len bytes during the call.
     * @param salt_len Salt length in bytes.
     * @param iterations Iteration count. Zero returns an empty result.
     * @param output_len Requested derived-key length in bytes.
     * @return Derived key bytes. Empty when iterations or output_len is zero.
     */
    static std::vector<uint8_t> hmacSha256(const uint8_t* password,
                                           size_t password_len,
                                           const uint8_t* salt,
                                           size_t salt_len,
                                           uint32_t iterations,
                                           size_t output_len);

    /**
     * @brief Derive key bytes with PBKDF2-HMAC-SHA256 using string password and byte salt.
     */
    static std::vector<uint8_t> hmacSha256(const std::string& password,
                                           const std::vector<uint8_t>& salt,
                                           uint32_t iterations,
                                           size_t output_len);
};

inline std::vector<uint8_t> PBKDF2::hmacSha256(const uint8_t* password,
                                               size_t password_len,
                                               const uint8_t* salt,
                                               size_t salt_len,
                                               uint32_t iterations,
                                               size_t output_len)
{
    if (iterations == 0 || output_len == 0) {
        return {};
    }

    constexpr size_t kDigestSize = 32;
    const uint32_t block_count = static_cast<uint32_t>((output_len + kDigestSize - 1) / kDigestSize);
    std::vector<uint8_t> output;
    output.reserve(static_cast<size_t>(block_count) * kDigestSize);

    std::vector<uint8_t> block_input(salt_len + 4);
    if (salt_len > 0) {
        std::memcpy(block_input.data(), salt, salt_len);
    }

    for (uint32_t block_index = 1; block_index <= block_count; ++block_index) {
        block_input[salt_len] = static_cast<uint8_t>((block_index >> 24) & 0xff);
        block_input[salt_len + 1] = static_cast<uint8_t>((block_index >> 16) & 0xff);
        block_input[salt_len + 2] = static_cast<uint8_t>((block_index >> 8) & 0xff);
        block_input[salt_len + 3] = static_cast<uint8_t>(block_index & 0xff);

        auto u = HMAC::hmacSha256(password, password_len, block_input.data(), block_input.size());
        std::array<uint8_t, kDigestSize> t = u;

        for (uint32_t iteration = 1; iteration < iterations; ++iteration) {
            u = HMAC::hmacSha256(password, password_len, u.data(), u.size());
            for (size_t i = 0; i < kDigestSize; ++i) {
                t[i] ^= u[i];
            }
        }

        output.insert(output.end(), t.begin(), t.end());
    }

    output.resize(output_len);
    return output;
}

inline std::vector<uint8_t> PBKDF2::hmacSha256(const std::string& password,
                                               const std::vector<uint8_t>& salt,
                                               uint32_t iterations,
                                               size_t output_len)
{
    return hmacSha256(reinterpret_cast<const uint8_t*>(password.data()),
                      password.size(),
                      salt.data(),
                      salt.size(),
                      iterations,
                      output_len);
}

} // namespace galay::utils

#endif // GALAY_UTILS_PBKDF2_HPP
