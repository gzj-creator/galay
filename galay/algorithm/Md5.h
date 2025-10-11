#ifndef __GALAY_MD5_H__
#define __GALAY_MD5_H__

#include <string>
#include <cstring>
#include <sstream>
#include <iomanip>
#if __cplusplus >= 201703L
#include <string_view>
#endif

namespace galay::algorithm
{
    /**
     * @brief MD5哈希工具类
     * @details 提供MD5消息摘要算法，生成128位（16字节）哈希值
     */
    class Md5Util
    {
    public:
        /**
         * @brief 计算字符串的MD5哈希值
         * @param str 待哈希的字符串
         * @return 32位十六进制字符串表示的MD5哈希值
         */
        static std::string encode(std::string const &str);
#if __cplusplus >= 201703L
        /**
         * @brief 计算字符串视图的MD5哈希值（C++17）
         * @param str 待哈希的字符串视图
         * @return 32位十六进制字符串表示的MD5哈希值
         */
        static std::string encode(std::string_view str);
#endif
    };
}

#endif