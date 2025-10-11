#ifndef __GALAY_SHA256_H__
#define __GALAY_SHA256_H__


#include <string>
#include <cstring>
#include <sstream>
#include <iomanip>
#if __cplusplus >= 201703L
#include <string_view>
#endif 
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace galay::algorithm
{
    /**
     * @brief SHA256哈希工具类
     * @details 提供SHA-256安全哈希算法，生成256位（32字节）哈希值
     *          使用OpenSSL库实现
     */
    class Sha256Util
    {
    public:
        /**
         * @brief 计算字符串的SHA256哈希值
         * @param str 待哈希的字符串
         * @return 64位十六进制字符串表示的SHA256哈希值
         */
        static std::string encode(const std::string &str);
#if __cplusplus >= 201703L
        /**
         * @brief 计算字符串视图的SHA256哈希值（C++17）
         * @param str 待哈希的字符串视图
         * @return 64位十六进制字符串表示的SHA256哈希值
         */
        static std::string encode(std::string_view str);
#endif
    };

}

#endif