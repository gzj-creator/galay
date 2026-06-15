/**
 * @file md5.hpp
 * @brief MD5 哈希算法实现
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供 MD5 摘要计算功能，支持字符串和原始字节数据输入，
 *          返回十六进制字符串或 16 字节原始哈希值。支持 C++17 的 string_view 接口。
 */

#ifndef GALAY_UTILS_MD5_H
#define GALAY_UTILS_MD5_H

#include <string>
#include <cstring>
#include <cstdint>
#include <array>

#if __cplusplus >= 201703L
#include <string_view>
#endif

namespace galay::utils
{
    /**
     * @brief MD5 哈希工具类
     * @details 提供 MD5 摘要计算，返回十六进制字符串或 16 字节原始哈希值。
     */
    class MD5Util
    {
    public:
        /**
         * @brief 计算字符串的 MD5 哈希值（十六进制格式）
         * @param input 输入字符串
         * @return 32 字符的十六进制 MD5 字符串
         */
        static std::string MD5(const std::string& input);

        /**
         * @brief 计算原始字节的 MD5 哈希值（十六进制格式）
         * @param data 输入数据指针
         * @param length 数据长度
         * @return 32 字符的十六进制 MD5 字符串
         */
        static std::string MD5(const unsigned char* data, size_t length);

        /**
         * @brief 计算字符串的 MD5 哈希值（原始字节）
         * @param input 输入字符串
         * @return 16 字节的 MD5 哈希数组
         */
        static std::array<uint8_t, 16> MD5Raw(const std::string& input);

        /**
         * @brief 计算原始字节的 MD5 哈希值（原始字节）
         * @param data 输入数据指针
         * @param length 数据长度
         * @return 16 字节的 MD5 哈希数组
         */
        static std::array<uint8_t, 16> MD5Raw(const unsigned char* data, size_t length);

#if __cplusplus >= 201703L
        /**
         * @brief 计算字符串视图的 MD5 哈希值（十六进制格式，C++17）
         * @param input 输入字符串视图
         * @return 32 字符的十六进制 MD5 字符串
         */
        static std::string MD5View(std::string_view input);

        /**
         * @brief 计算字符串视图的 MD5 哈希值（原始字节，C++17）
         * @param input 输入字符串视图
         * @return 16 字节的 MD5 哈希数组
         */
        static std::array<uint8_t, 16> MD5RawView(std::string_view input);
#endif

    private:
        // MD5 context for incremental hashing
        struct Context
        {
            uint32_t state[4];      // ABCD
            uint32_t count[2];      // Number of bits, modulo 2^64 (lsb first)
            uint8_t buffer[64];     // Input buffer
        };

        // MD5 constants
        static constexpr uint32_t S11 = 7;
        static constexpr uint32_t S12 = 12;
        static constexpr uint32_t S13 = 17;
        static constexpr uint32_t S14 = 22;
        static constexpr uint32_t S21 = 5;
        static constexpr uint32_t S22 = 9;
        static constexpr uint32_t S23 = 14;
        static constexpr uint32_t S24 = 20;
        static constexpr uint32_t S31 = 4;
        static constexpr uint32_t S32 = 11;
        static constexpr uint32_t S33 = 16;
        static constexpr uint32_t S34 = 23;
        static constexpr uint32_t S41 = 6;
        static constexpr uint32_t S42 = 10;
        static constexpr uint32_t S43 = 15;
        static constexpr uint32_t S44 = 21;

        // MD5 basic transformation functions
        static inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
        static inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
        static inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
        static inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

        // Rotate left
        static inline uint32_t rotateLeft(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

        // FF, GG, HH, and II transformations
        static inline void FF(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac)
        {
            a += F(b, c, d) + x + ac;
            a = rotateLeft(a, s);
            a += b;
        }

        static inline void GG(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac)
        {
            a += G(b, c, d) + x + ac;
            a = rotateLeft(a, s);
            a += b;
        }

        static inline void HH(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac)
        {
            a += H(b, c, d) + x + ac;
            a = rotateLeft(a, s);
            a += b;
        }

        static inline void II(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac)
        {
            a += I(b, c, d) + x + ac;
            a = rotateLeft(a, s);
            a += b;
        }

        // Core MD5 functions
        static void init(Context& ctx);
        static void update(Context& ctx, const uint8_t* input, size_t length);
        static void finalize(Context& ctx, uint8_t digest[16]);
        static void transform(uint32_t state[4], const uint8_t block[64]);

        // Utility functions
        static void encode(uint8_t* output, const uint32_t* input, size_t length);
        static void decode(uint32_t* output, const uint8_t* input, size_t length);
        static std::string toHexString(const uint8_t* data, size_t length);
    };

    // Implementation

    inline void MD5Util::init(Context& ctx)
    {
        ctx.count[0] = 0;
        ctx.count[1] = 0;

        // Load magic initialization constants
        ctx.state[0] = 0x67452301;
        ctx.state[1] = 0xefcdab89;
        ctx.state[2] = 0x98badcfe;
        ctx.state[3] = 0x10325476;
    }

    inline void MD5Util::update(Context& ctx, const uint8_t* input, size_t length)
    {
        // Compute number of bytes mod 64
        uint32_t index = (ctx.count[0] >> 3) & 0x3F;

        // Update number of bits
        if ((ctx.count[0] += (length << 3)) < (length << 3))
            ctx.count[1]++;
        ctx.count[1] += (length >> 29);

        uint32_t partLen = 64 - index;

        // Transform as many times as possible
        uint32_t i = 0;
        if (length >= partLen)
        {
            std::memcpy(&ctx.buffer[index], input, partLen);
            transform(ctx.state, ctx.buffer);

            for (i = partLen; i + 63 < length; i += 64)
                transform(ctx.state, &input[i]);

            index = 0;
        }

        // Buffer remaining input
        std::memcpy(&ctx.buffer[index], &input[i], length - i);
    }

    inline void MD5Util::finalize(Context& ctx, uint8_t digest[16])
    {
        static const uint8_t padding[64] = {
            0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        };

        // Save number of bits
        uint8_t bits[8];
        encode(bits, ctx.count, 8);

        // Pad out to 56 mod 64
        uint32_t index = (ctx.count[0] >> 3) & 0x3f;
        uint32_t padLen = (index < 56) ? (56 - index) : (120 - index);
        update(ctx, padding, padLen);

        // Append length (before padding)
        update(ctx, bits, 8);

        // Store state in digest
        encode(digest, ctx.state, 16);
    }

    inline void MD5Util::transform(uint32_t state[4], const uint8_t block[64])
    {
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t x[16];

        decode(x, block, 64);

        // Round 1
        FF(a, b, c, d, x[0], S11, 0xd76aa478);
        FF(d, a, b, c, x[1], S12, 0xe8c7b756);
        FF(c, d, a, b, x[2], S13, 0x242070db);
        FF(b, c, d, a, x[3], S14, 0xc1bdceee);
        FF(a, b, c, d, x[4], S11, 0xf57c0faf);
        FF(d, a, b, c, x[5], S12, 0x4787c62a);
        FF(c, d, a, b, x[6], S13, 0xa8304613);
        FF(b, c, d, a, x[7], S14, 0xfd469501);
        FF(a, b, c, d, x[8], S11, 0x698098d8);
        FF(d, a, b, c, x[9], S12, 0x8b44f7af);
        FF(c, d, a, b, x[10], S13, 0xffff5bb1);
        FF(b, c, d, a, x[11], S14, 0x895cd7be);
        FF(a, b, c, d, x[12], S11, 0x6b901122);
        FF(d, a, b, c, x[13], S12, 0xfd987193);
        FF(c, d, a, b, x[14], S13, 0xa679438e);
        FF(b, c, d, a, x[15], S14, 0x49b40821);

        // Round 2
        GG(a, b, c, d, x[1], S21, 0xf61e2562);
        GG(d, a, b, c, x[6], S22, 0xc040b340);
        GG(c, d, a, b, x[11], S23, 0x265e5a51);
        GG(b, c, d, a, x[0], S24, 0xe9b6c7aa);
        GG(a, b, c, d, x[5], S21, 0xd62f105d);
        GG(d, a, b, c, x[10], S22, 0x2441453);
        GG(c, d, a, b, x[15], S23, 0xd8a1e681);
        GG(b, c, d, a, x[4], S24, 0xe7d3fbc8);
        GG(a, b, c, d, x[9], S21, 0x21e1cde6);
        GG(d, a, b, c, x[14], S22, 0xc33707d6);
        GG(c, d, a, b, x[3], S23, 0xf4d50d87);
        GG(b, c, d, a, x[8], S24, 0x455a14ed);
        GG(a, b, c, d, x[13], S21, 0xa9e3e905);
        GG(d, a, b, c, x[2], S22, 0xfcefa3f8);
        GG(c, d, a, b, x[7], S23, 0x676f02d9);
        GG(b, c, d, a, x[12], S24, 0x8d2a4c8a);

        // Round 3
        HH(a, b, c, d, x[5], S31, 0xfffa3942);
        HH(d, a, b, c, x[8], S32, 0x8771f681);
        HH(c, d, a, b, x[11], S33, 0x6d9d6122);
        HH(b, c, d, a, x[14], S34, 0xfde5380c);
        HH(a, b, c, d, x[1], S31, 0xa4beea44);
        HH(d, a, b, c, x[4], S32, 0x4bdecfa9);
        HH(c, d, a, b, x[7], S33, 0xf6bb4b60);
        HH(b, c, d, a, x[10], S34, 0xbebfbc70);
        HH(a, b, c, d, x[13], S31, 0x289b7ec6);
        HH(d, a, b, c, x[0], S32, 0xeaa127fa);
        HH(c, d, a, b, x[3], S33, 0xd4ef3085);
        HH(b, c, d, a, x[6], S34, 0x4881d05);
        HH(a, b, c, d, x[9], S31, 0xd9d4d039);
        HH(d, a, b, c, x[12], S32, 0xe6db99e5);
        HH(c, d, a, b, x[15], S33, 0x1fa27cf8);
        HH(b, c, d, a, x[2], S34, 0xc4ac5665);

        // Round 4
        II(a, b, c, d, x[0], S41, 0xf4292244);
        II(d, a, b, c, x[7], S42, 0x432aff97);
        II(c, d, a, b, x[14], S43, 0xab9423a7);
        II(b, c, d, a, x[5], S44, 0xfc93a039);
        II(a, b, c, d, x[12], S41, 0x655b59c3);
        II(d, a, b, c, x[3], S42, 0x8f0ccc92);
        II(c, d, a, b, x[10], S43, 0xffeff47d);
        II(b, c, d, a, x[1], S44, 0x85845dd1);
        II(a, b, c, d, x[8], S41, 0x6fa87e4f);
        II(d, a, b, c, x[15], S42, 0xfe2ce6e0);
        II(c, d, a, b, x[6], S43, 0xa3014314);
        II(b, c, d, a, x[13], S44, 0x4e0811a1);
        II(a, b, c, d, x[4], S41, 0xf7537e82);
        II(d, a, b, c, x[11], S42, 0xbd3af235);
        II(c, d, a, b, x[2], S43, 0x2ad7d2bb);
        II(b, c, d, a, x[9], S44, 0xeb86d391);

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }

    inline void MD5Util::encode(uint8_t* output, const uint32_t* input, size_t length)
    {
        for (size_t i = 0, j = 0; j < length; i++, j += 4)
        {
            output[j] = input[i] & 0xff;
            output[j + 1] = (input[i] >> 8) & 0xff;
            output[j + 2] = (input[i] >> 16) & 0xff;
            output[j + 3] = (input[i] >> 24) & 0xff;
        }
    }

    inline void MD5Util::decode(uint32_t* output, const uint8_t* input, size_t length)
    {
        for (size_t i = 0, j = 0; j < length; i++, j += 4)
        {
            output[i] = static_cast<uint32_t>(input[j]) |
                       (static_cast<uint32_t>(input[j + 1]) << 8) |
                       (static_cast<uint32_t>(input[j + 2]) << 16) |
                       (static_cast<uint32_t>(input[j + 3]) << 24);
        }
    }

    inline std::string MD5Util::toHexString(const uint8_t* data, size_t length)
    {
        static const char hexChars[] = "0123456789abcdef";
        std::string result;
        result.reserve(length * 2);

        for (size_t i = 0; i < length; ++i)
        {
            result.push_back(hexChars[data[i] >> 4]);
            result.push_back(hexChars[data[i] & 0x0F]);
        }

        return result;
    }

    inline std::array<uint8_t, 16> MD5Util::MD5Raw(const unsigned char* data, size_t length)
    {
        Context ctx;
        init(ctx);
        update(ctx, data, length);

        std::array<uint8_t, 16> digest;
        finalize(ctx, digest.data());

        return digest;
    }

    inline std::array<uint8_t, 16> MD5Util::MD5Raw(const std::string& input)
    {
        return MD5Raw(reinterpret_cast<const unsigned char*>(input.data()), input.length());
    }

    inline std::string MD5Util::MD5(const unsigned char* data, size_t length)
    {
        auto digest = MD5Raw(data, length);
        return toHexString(digest.data(), digest.size());
    }

    inline std::string MD5Util::MD5(const std::string& input)
    {
        return MD5(reinterpret_cast<const unsigned char*>(input.data()), input.length());
    }

#if __cplusplus >= 201703L
    inline std::string MD5Util::MD5View(std::string_view input)
    {
        return MD5(reinterpret_cast<const unsigned char*>(input.data()), input.length());
    }

    inline std::array<uint8_t, 16> MD5Util::MD5RawView(std::string_view input)
    {
        return MD5Raw(reinterpret_cast<const unsigned char*>(input.data()), input.length());
    }
#endif

} // namespace galay::utils

#endif /* GALAY_UTILS_MD5_H */
