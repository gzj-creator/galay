/**
 * @file hmac.hpp
 * @brief SHA-256 和 HMAC-SHA256 算法实现
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供 SHA-256 哈希计算和基于 SHA-256 的 HMAC 消息认证码生成功能。
 *          纯头文件实现，不依赖外部加密库。
 */

#ifndef GALAY_UTILS_HMAC_H
#define GALAY_UTILS_HMAC_H

#include <string>
#include <cstring>
#include <cstdint>
#include <array>
#include <vector>

namespace galay::utils
{
    /**
     * @brief SHA-256 哈希算法实现
     * @details 提供 SHA-256 哈希计算，返回原始字节数组或十六进制字符串。
     */
    class SHA256
    {
    public:
        /**
         * @brief 计算 SHA-256 哈希值
         * @param data 输入数据指针
         * @param length 数据长度
         * @return 32 字节的哈希值数组
         */
        static std::array<uint8_t, 32> hash(const uint8_t* data, size_t length);

        /**
         * @brief 计算并返回十六进制格式的 SHA-256 哈希值
         * @param data 输入数据指针
         * @param length 数据长度
         * @return 64 字符的十六进制哈希字符串
         */
        static std::string hashHex(const uint8_t* data, size_t length);

        /**
         * @brief 计算并返回十六进制格式的 SHA-256 哈希值
         * @param data 输入字符串
         * @return 64 字符的十六进制哈希字符串
         */
        static std::string hashHex(const std::string& data);

    private:
        static constexpr uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
        static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
        static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
        static inline uint32_t sigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
        static inline uint32_t sigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
        static inline uint32_t gamma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
        static inline uint32_t gamma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

        static void transform(uint32_t state[8], const uint8_t block[64]);
    };

    /**
     * @brief HMAC-SHA256 消息认证码实现
     * @details 提供基于 SHA-256 的 HMAC 计算，用于消息完整性和认证。
     */
    class HMAC
    {
    public:
        /**
         * @brief 计算 HMAC-SHA256
         * @param key 密钥指针
         * @param keyLen 密钥长度
         * @param data 数据指针
         * @param dataLen 数据长度
         * @return 32 字节的 HMAC 值数组
         */
        static std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t keyLen,
                                                   const uint8_t* data, size_t dataLen);

        /**
         * @brief 计算并返回十六进制格式的 HMAC-SHA256
         * @param key 密钥字符串
         * @param data 数据字符串
         * @return 64 字符的十六进制 HMAC 字符串
         */
        static std::string hmacSha256Hex(const std::string& key, const std::string& data);

        /**
         * @brief 计算 HMAC-SHA256
         * @param key 密钥字符串
         * @param data 数据字符串
         * @return 32 字节的 HMAC 值数组
         */
        static std::array<uint8_t, 32> hmacSha256(const std::string& key, const std::string& data);
    };

    // Implementation

    inline void SHA256::transform(uint32_t state[8], const uint8_t block[64])
    {
        uint32_t w[64];
        uint32_t a, b, c, d, e, f, g, h;

        // Prepare message schedule
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]));
        }

        for (int i = 16; i < 64; ++i)
        {
            w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
        }

        // Initialize working variables
        a = state[0];
        b = state[1];
        c = state[2];
        d = state[3];
        e = state[4];
        f = state[5];
        g = state[6];
        h = state[7];

        // Main loop
        for (int i = 0; i < 64; ++i)
        {
            uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K[i] + w[i];
            uint32_t t2 = sigma0(a) + maj(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        // Add compressed chunk to current hash value
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    inline std::array<uint8_t, 32> SHA256::hash(const uint8_t* data, size_t length)
    {
        uint32_t state[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };

        size_t totalBits = length * 8;
        size_t paddedLength = ((length + 8) / 64 + 1) * 64;

        std::vector<uint8_t> padded(paddedLength, 0);
        std::memcpy(padded.data(), data, length);

        // Append '1' bit
        padded[length] = 0x80;

        // Append length in bits as 64-bit big-endian
        for (int i = 0; i < 8; ++i)
        {
            padded[paddedLength - 1 - i] = (totalBits >> (i * 8)) & 0xFF;
        }

        // Process blocks
        for (size_t i = 0; i < paddedLength; i += 64)
        {
            transform(state, &padded[i]);
        }

        // Produce final hash
        std::array<uint8_t, 32> result;
        for (int i = 0; i < 8; ++i)
        {
            result[i * 4] = (state[i] >> 24) & 0xFF;
            result[i * 4 + 1] = (state[i] >> 16) & 0xFF;
            result[i * 4 + 2] = (state[i] >> 8) & 0xFF;
            result[i * 4 + 3] = state[i] & 0xFF;
        }

        return result;
    }

    inline std::string SHA256::hashHex(const uint8_t* data, size_t length)
    {
        auto hashBytes = hash(data, length);

        static const char hexChars[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);

        for (uint8_t byte : hashBytes)
        {
            result.push_back(hexChars[byte >> 4]);
            result.push_back(hexChars[byte & 0x0F]);
        }

        return result;
    }

    inline std::string SHA256::hashHex(const std::string& data)
    {
        return hashHex(reinterpret_cast<const uint8_t*>(data.data()), data.length());
    }

    inline std::array<uint8_t, 32> HMAC::hmacSha256(const uint8_t* key, size_t keyLen,
                                                     const uint8_t* data, size_t dataLen)
    {
        constexpr size_t blockSize = 64;
        uint8_t keyBlock[blockSize] = {0};

        // If key is longer than block size, hash it
        if (keyLen > blockSize)
        {
            auto hashedKey = SHA256::hash(key, keyLen);
            std::memcpy(keyBlock, hashedKey.data(), 32);
        }
        else
        {
            std::memcpy(keyBlock, key, keyLen);
        }

        // Create inner and outer padded keys
        uint8_t ipad[blockSize];
        uint8_t opad[blockSize];

        for (size_t i = 0; i < blockSize; ++i)
        {
            ipad[i] = keyBlock[i] ^ 0x36;
            opad[i] = keyBlock[i] ^ 0x5c;
        }

        // Inner hash: H(K XOR ipad, text)
        std::vector<uint8_t> innerData(blockSize + dataLen);
        std::memcpy(innerData.data(), ipad, blockSize);
        std::memcpy(innerData.data() + blockSize, data, dataLen);
        auto innerHash = SHA256::hash(innerData.data(), innerData.size());

        // Outer hash: H(K XOR opad, innerHash)
        std::vector<uint8_t> outerData(blockSize + 32);
        std::memcpy(outerData.data(), opad, blockSize);
        std::memcpy(outerData.data() + blockSize, innerHash.data(), 32);

        return SHA256::hash(outerData.data(), outerData.size());
    }

    inline std::array<uint8_t, 32> HMAC::hmacSha256(const std::string& key, const std::string& data)
    {
        return hmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.length(),
                         reinterpret_cast<const uint8_t*>(data.data()), data.length());
    }

    inline std::string HMAC::hmacSha256Hex(const std::string& key, const std::string& data)
    {
        auto hmac = hmacSha256(key, data);

        static const char hexChars[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);

        for (uint8_t byte : hmac)
        {
            result.push_back(hexChars[byte >> 4]);
            result.push_back(hexChars[byte & 0x0F]);
        }

        return result;
    }

} // namespace galay::utils

#endif /* GALAY_UTILS_HMAC_H */
