#ifndef GALAY_UTILS_SHA1_HPP
#define GALAY_UTILS_SHA1_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace galay::utils
{
    class SHA1
    {
    public:
        /**
         * @brief Compute the SHA-1 digest for a byte range.
         * @param data Non-owning input pointer. It must remain valid for the duration of the call.
         * @param length Number of bytes to hash.
         * @return 20-byte SHA-1 digest.
         */
        static std::array<uint8_t, 20> hash(const uint8_t* data, size_t length)
        {
            Context ctx;
            init(ctx);
            update(ctx, data, length);
            std::array<uint8_t, 20> digest{};
            finalize(ctx, digest.data());
            return digest;
        }

        static std::string hashHex(const uint8_t* data, size_t length)
        {
            const auto digest = hash(data, length);
            return toHex(digest.data(), digest.size());
        }

        static std::string hashHex(const std::string& data)
        {
            return hashHex(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        }

    private:
        struct Context
        {
            uint32_t state[5];
            uint64_t bit_count;
            uint8_t buffer[64];
            size_t buffer_size;
        };

        static constexpr std::array<uint32_t, 4> kRoundConstants{
            0x5A827999U,
            0x6ED9EBA1U,
            0x8F1BBCDCU,
            0xCA62C1D6U
        };

        static inline uint32_t leftRotate(uint32_t value, uint32_t bits)
        {
            return (value << bits) | (value >> (32U - bits));
        }

        static inline void init(Context& ctx)
        {
            ctx.state[0] = 0x67452301U;
            ctx.state[1] = 0xEFCDAB89U;
            ctx.state[2] = 0x98BADCFEU;
            ctx.state[3] = 0x10325476U;
            ctx.state[4] = 0xC3D2E1F0U;
            ctx.bit_count = 0;
            ctx.buffer_size = 0;
        }

        static inline void update(Context& ctx, const uint8_t* data, size_t length)
        {
            ctx.bit_count += static_cast<uint64_t>(length) * 8U;

            size_t offset = 0;
            while (offset < length) {
                const size_t copy_len = (length - offset < 64U - ctx.buffer_size)
                    ? (length - offset)
                    : (64U - ctx.buffer_size);
                std::memcpy(ctx.buffer + ctx.buffer_size, data + offset, copy_len);
                ctx.buffer_size += copy_len;
                offset += copy_len;

                if (ctx.buffer_size == 64U) {
                    transform(ctx.state, ctx.buffer);
                    ctx.buffer_size = 0;
                }
            }
        }

        static inline void finalize(Context& ctx, uint8_t digest[20])
        {
            ctx.buffer[ctx.buffer_size++] = 0x80U;

            if (ctx.buffer_size > 56U) {
                std::memset(ctx.buffer + ctx.buffer_size, 0, 64U - ctx.buffer_size);
                transform(ctx.state, ctx.buffer);
                ctx.buffer_size = 0;
            }

            std::memset(ctx.buffer + ctx.buffer_size, 0, 56U - ctx.buffer_size);
            ctx.buffer_size = 56U;

            for (size_t i = 0; i < 8; ++i) {
                ctx.buffer[56U + i] = static_cast<uint8_t>(ctx.bit_count >> ((7U - i) * 8U));
            }

            transform(ctx.state, ctx.buffer);

            for (size_t i = 0; i < 5; ++i) {
                digest[i * 4] = static_cast<uint8_t>(ctx.state[i] >> 24U);
                digest[i * 4 + 1] = static_cast<uint8_t>(ctx.state[i] >> 16U);
                digest[i * 4 + 2] = static_cast<uint8_t>(ctx.state[i] >> 8U);
                digest[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i]);
            }
        }

        static inline void transform(uint32_t state[5], const uint8_t block[64])
        {
            uint32_t schedule[80];
            for (size_t i = 0; i < 16; ++i) {
                const size_t base = i * 4;
                schedule[i] =
                    (static_cast<uint32_t>(block[base]) << 24U) |
                    (static_cast<uint32_t>(block[base + 1]) << 16U) |
                    (static_cast<uint32_t>(block[base + 2]) << 8U) |
                    static_cast<uint32_t>(block[base + 3]);
            }

            for (size_t i = 16; i < 80; ++i) {
                schedule[i] = leftRotate(
                    schedule[i - 3] ^ schedule[i - 8] ^ schedule[i - 14] ^ schedule[i - 16], 1U);
            }

            uint32_t a = state[0];
            uint32_t b = state[1];
            uint32_t c = state[2];
            uint32_t d = state[3];
            uint32_t e = state[4];

            for (size_t i = 0; i < 80; ++i) {
                uint32_t f = 0;
                uint32_t k = 0;
                if (i < 20) {
                    f = (b & c) | ((~b) & d);
                    k = kRoundConstants[0];
                } else if (i < 40) {
                    f = b ^ c ^ d;
                    k = kRoundConstants[1];
                } else if (i < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = kRoundConstants[2];
                } else {
                    f = b ^ c ^ d;
                    k = kRoundConstants[3];
                }

                const uint32_t temp = leftRotate(a, 5U) + f + e + k + schedule[i];
                e = d;
                d = c;
                c = leftRotate(b, 30U);
                b = a;
                a = temp;
            }

            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
        }

        static inline std::string toHex(const uint8_t* data, size_t length)
        {
            static constexpr char kHexDigits[] = "0123456789abcdef";
            std::string result;
            result.resize(length * 2U);
            for (size_t i = 0; i < length; ++i) {
                result[i * 2] = kHexDigits[(data[i] >> 4U) & 0x0FU];
                result[i * 2 + 1] = kHexDigits[data[i] & 0x0FU];
            }
            return result;
        }
    };
}

#endif
