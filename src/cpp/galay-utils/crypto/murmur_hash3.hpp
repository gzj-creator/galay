/**
 * @file murmur_hash3.hpp
 * @brief MurmurHash3 哈希算法实现
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供 MurmurHash3 的 32 位和 128 位哈希计算功能，
 *          支持字符串和原始数据输入，可选种子值。支持 C++17 的 string_view 接口。
 */

#ifndef GALAY_UTILS_MURMURHASH3_H
#define GALAY_UTILS_MURMURHASH3_H

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
     * @brief MurmurHash3 哈希工具类
     * @details 提供 MurmurHash3 的 32 位和 128 位哈希计算，
     *          支持原始数据和字符串输入，可选种子值。
     */
    class MurmurHash3Util
    {
    public:
        /**
         * @brief 计算 32 位 MurmurHash3 哈希值
         * @param key 输入数据指针
         * @param len 数据长度
         * @param seed 哈希种子值
         * @return 32 位哈希值
         */
        static uint32_t Hash32(const void* key, size_t len, uint32_t seed = 0);

        /**
         * @brief 计算字符串的 32 位 MurmurHash3 哈希值
         * @param str 输入字符串
         * @param seed 哈希种子值
         * @return 32 位哈希值
         */
        static uint32_t Hash32(const std::string& str, uint32_t seed = 0);

        /**
         * @brief 计算 C 字符串的 32 位 MurmurHash3 哈希值
         * @param str 以 '\0' 结尾的输入字符串；为 nullptr 时按空串处理
         * @param seed 哈希种子值
         * @return 32 位哈希值
         */
        static uint32_t Hash32(const char* str, uint32_t seed = 0);

        /**
         * @brief 计算 128 位 MurmurHash3 哈希值（十六进制字符串）
         * @param key 输入数据指针
         * @param len 数据长度
         * @param seed 哈希种子值
         * @return 32 字符的十六进制哈希字符串
         */
        static std::string Hash128(const void* key, size_t len, uint32_t seed = 0);

        /**
         * @brief 计算字符串的 128 位 MurmurHash3 哈希值（十六进制字符串）
         * @param str 输入字符串
         * @param seed 哈希种子值
         * @return 32 字符的十六进制哈希字符串
         */
        static std::string Hash128(const std::string& str, uint32_t seed = 0);

        /**
         * @brief 计算 C 字符串的 128 位 MurmurHash3 哈希值（十六进制字符串）
         * @param str 以 '\0' 结尾的输入字符串；为 nullptr 时按空串处理
         * @param seed 哈希种子值
         * @return 32 字符的十六进制哈希字符串
         */
        static std::string Hash128(const char* str, uint32_t seed = 0);

        /**
         * @brief 计算 128 位 MurmurHash3 哈希值（原始字节）
         * @param key 输入数据指针
         * @param len 数据长度
         * @param seed 哈希种子值
         * @return 包含两个 64 位整数的数组
         */
        static std::array<uint64_t, 2> Hash128Raw(const void* key, size_t len, uint32_t seed = 0);

        /**
         * @brief 计算字符串的 128 位 MurmurHash3 哈希值（原始字节）
         * @param str 输入字符串
         * @param seed 哈希种子值
         * @return 包含两个 64 位整数的数组
         */
        static std::array<uint64_t, 2> Hash128Raw(const std::string& str, uint32_t seed = 0);

        /**
         * @brief 计算 C 字符串的 128 位 MurmurHash3 哈希值（原始字节）
         * @param str 以 '\0' 结尾的输入字符串；为 nullptr 时按空串处理
         * @param seed 哈希种子值
         * @return 包含两个 64 位整数的数组
         */
        static std::array<uint64_t, 2> Hash128Raw(const char* str, uint32_t seed = 0);

#if __cplusplus >= 201703L
        /**
         * @brief 计算字符串视图的 32 位哈希值（C++17）
         * @param str 输入字符串视图
         * @param seed 哈希种子值
         * @return 32 位哈希值
         */
        static uint32_t Hash32View(std::string_view str, uint32_t seed = 0);

        /**
         * @brief 计算字符串视图的 128 位哈希值（十六进制，C++17）
         * @param str 输入字符串视图
         * @param seed 哈希种子值
         * @return 32 字符的十六进制哈希字符串
         */
        static std::string Hash128View(std::string_view str, uint32_t seed = 0);

        /**
         * @brief 计算字符串视图的 128 位哈希值（原始字节，C++17）
         * @param str 输入字符串视图
         * @param seed 哈希种子值
         * @return 包含两个 64 位整数的数组
         */
        static std::array<uint64_t, 2> Hash128RawView(std::string_view str, uint32_t seed = 0);
#endif

    private:
        // Helper functions for 32-bit hash
        static inline uint32_t rotl32(uint32_t x, int8_t r)
        {
            return (x << r) | (x >> (32 - r));
        }

        static inline uint32_t fmix32(uint32_t h)
        {
            h ^= h >> 16;
            h *= 0x85ebca6b;
            h ^= h >> 13;
            h *= 0xc2b2ae35;
            h ^= h >> 16;
            return h;
        }

        // Helper functions for 64-bit hash
        static inline uint64_t rotl64(uint64_t x, int8_t r)
        {
            return (x << r) | (x >> (64 - r));
        }

        static inline uint64_t fmix64(uint64_t k)
        {
            k ^= k >> 33;
            k *= 0xff51afd7ed558ccdULL;
            k ^= k >> 33;
            k *= 0xc4ceb9fe1a85ec53ULL;
            k ^= k >> 33;
            return k;
        }

        // Utility function to convert to hex string
        static std::string toHexString(uint64_t high, uint64_t low);
    };

    // Implementation

    inline uint32_t MurmurHash3Util::Hash32(const void* key, size_t len, uint32_t seed)
    {
        const uint8_t* data = static_cast<const uint8_t*>(key);
        const int nblocks = len / 4;

        uint32_t h1 = seed;

        const uint32_t c1 = 0xcc9e2d51;
        const uint32_t c2 = 0x1b873593;

        // Body
        const uint32_t* blocks = reinterpret_cast<const uint32_t*>(data + nblocks * 4);

        for (int i = -nblocks; i; i++)
        {
            uint32_t k1 = blocks[i];

            k1 *= c1;
            k1 = rotl32(k1, 15);
            k1 *= c2;

            h1 ^= k1;
            h1 = rotl32(h1, 13);
            h1 = h1 * 5 + 0xe6546b64;
        }

        // Tail
        const uint8_t* tail = data + nblocks * 4;

        uint32_t k1 = 0;

        switch (len & 3)
        {
        case 3: k1 ^= tail[2] << 16; [[fallthrough]];
        case 2: k1 ^= tail[1] << 8;  [[fallthrough]];
        case 1: k1 ^= tail[0];
                k1 *= c1;
                k1 = rotl32(k1, 15);
                k1 *= c2;
                h1 ^= k1;
        }

        // Finalization
        h1 ^= len;
        h1 = fmix32(h1);

        return h1;
    }

    inline uint32_t MurmurHash3Util::Hash32(const std::string& str, uint32_t seed)
    {
        return Hash32(str.data(), str.length(), seed);
    }

    inline uint32_t MurmurHash3Util::Hash32(const char* str, uint32_t seed)
    {
        return str == nullptr ? Hash32("", 0, seed) : Hash32(str, std::strlen(str), seed);
    }

    inline std::array<uint64_t, 2> MurmurHash3Util::Hash128Raw(const void* key, size_t len, uint32_t seed)
    {
        const uint8_t* data = static_cast<const uint8_t*>(key);
        const int nblocks = len / 16;

        uint64_t h1 = seed;
        uint64_t h2 = seed;

        const uint64_t c1 = 0x87c37b91114253d5ULL;
        const uint64_t c2 = 0x4cf5ad432745937fULL;

        // Body
        const uint64_t* blocks = reinterpret_cast<const uint64_t*>(data);

        for (int i = 0; i < nblocks; i++)
        {
            uint64_t k1 = blocks[i * 2 + 0];
            uint64_t k2 = blocks[i * 2 + 1];

            k1 *= c1;
            k1 = rotl64(k1, 31);
            k1 *= c2;
            h1 ^= k1;

            h1 = rotl64(h1, 27);
            h1 += h2;
            h1 = h1 * 5 + 0x52dce729;

            k2 *= c2;
            k2 = rotl64(k2, 33);
            k2 *= c1;
            h2 ^= k2;

            h2 = rotl64(h2, 31);
            h2 += h1;
            h2 = h2 * 5 + 0x38495ab5;
        }

        // Tail
        const uint8_t* tail = data + nblocks * 16;

        uint64_t k1 = 0;
        uint64_t k2 = 0;

        switch (len & 15)
        {
        case 15: k2 ^= static_cast<uint64_t>(tail[14]) << 48; [[fallthrough]];
        case 14: k2 ^= static_cast<uint64_t>(tail[13]) << 40; [[fallthrough]];
        case 13: k2 ^= static_cast<uint64_t>(tail[12]) << 32; [[fallthrough]];
        case 12: k2 ^= static_cast<uint64_t>(tail[11]) << 24; [[fallthrough]];
        case 11: k2 ^= static_cast<uint64_t>(tail[10]) << 16; [[fallthrough]];
        case 10: k2 ^= static_cast<uint64_t>(tail[9]) << 8;   [[fallthrough]];
        case 9:  k2 ^= static_cast<uint64_t>(tail[8]) << 0;
                 k2 *= c2;
                 k2 = rotl64(k2, 33);
                 k2 *= c1;
                 h2 ^= k2;
                 [[fallthrough]];

        case 8:  k1 ^= static_cast<uint64_t>(tail[7]) << 56; [[fallthrough]];
        case 7:  k1 ^= static_cast<uint64_t>(tail[6]) << 48; [[fallthrough]];
        case 6:  k1 ^= static_cast<uint64_t>(tail[5]) << 40; [[fallthrough]];
        case 5:  k1 ^= static_cast<uint64_t>(tail[4]) << 32; [[fallthrough]];
        case 4:  k1 ^= static_cast<uint64_t>(tail[3]) << 24; [[fallthrough]];
        case 3:  k1 ^= static_cast<uint64_t>(tail[2]) << 16; [[fallthrough]];
        case 2:  k1 ^= static_cast<uint64_t>(tail[1]) << 8;  [[fallthrough]];
        case 1:  k1 ^= static_cast<uint64_t>(tail[0]) << 0;
                 k1 *= c1;
                 k1 = rotl64(k1, 31);
                 k1 *= c2;
                 h1 ^= k1;
        }

        // Finalization
        h1 ^= len;
        h2 ^= len;

        h1 += h2;
        h2 += h1;

        h1 = fmix64(h1);
        h2 = fmix64(h2);

        h1 += h2;
        h2 += h1;

        return {h1, h2};
    }

    inline std::array<uint64_t, 2> MurmurHash3Util::Hash128Raw(const std::string& str, uint32_t seed)
    {
        return Hash128Raw(str.data(), str.length(), seed);
    }

    inline std::array<uint64_t, 2> MurmurHash3Util::Hash128Raw(const char* str, uint32_t seed)
    {
        return str == nullptr ? Hash128Raw("", 0, seed) : Hash128Raw(str, std::strlen(str), seed);
    }

    inline std::string MurmurHash3Util::toHexString(uint64_t high, uint64_t low)
    {
        static const char hexChars[] = "0123456789abcdef";
        std::string result;
        result.reserve(32);

        // Convert high 64 bits
        for (int i = 60; i >= 0; i -= 4)
        {
            result.push_back(hexChars[(high >> i) & 0xF]);
        }

        // Convert low 64 bits
        for (int i = 60; i >= 0; i -= 4)
        {
            result.push_back(hexChars[(low >> i) & 0xF]);
        }

        return result;
    }

    inline std::string MurmurHash3Util::Hash128(const void* key, size_t len, uint32_t seed)
    {
        auto result = Hash128Raw(key, len, seed);
        return toHexString(result[0], result[1]);
    }

    inline std::string MurmurHash3Util::Hash128(const std::string& str, uint32_t seed)
    {
        return Hash128(str.data(), str.length(), seed);
    }

    inline std::string MurmurHash3Util::Hash128(const char* str, uint32_t seed)
    {
        return str == nullptr ? Hash128("", 0, seed) : Hash128(str, std::strlen(str), seed);
    }

#if __cplusplus >= 201703L
    inline uint32_t MurmurHash3Util::Hash32View(std::string_view str, uint32_t seed)
    {
        return Hash32(str.data(), str.length(), seed);
    }

    inline std::string MurmurHash3Util::Hash128View(std::string_view str, uint32_t seed)
    {
        return Hash128(str.data(), str.length(), seed);
    }

    inline std::array<uint64_t, 2> MurmurHash3Util::Hash128RawView(std::string_view str, uint32_t seed)
    {
        return Hash128Raw(str.data(), str.length(), seed);
    }
#endif

} // namespace galay::utils

#endif /* GALAY_UTILS_MURMURHASH3_H */
