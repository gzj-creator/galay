#ifndef __GALAY_SHA512_H__
#define __GALAY_SHA512_H__

#include <string>
#include <openssl/sha.h>

namespace galay::algorithm
{
    /**
     * @brief SHA512哈希工具类
     * @details 提供SHA-512安全哈希算法，生成512位（64字节）哈希值
     *          使用OpenSSL库实现，安全强度高于SHA256
     */
    class Sha512Util{
    public:
        /**
         * @brief 计算字符串的SHA512哈希值
         * @param str 待哈希的字符串
         * @return 128位十六进制字符串表示的SHA512哈希值
         */
        static std::string encode(const std::string& str);
    };
}



#endif