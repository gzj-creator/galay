/**
 * @file base64.hpp
 * @brief Base64 编解码工具
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供 Base64 编码和解码功能，支持标准 Base64 和 URL 安全 Base64 变体，
 *          以及 PEM 和 MIME 格式的编码。支持 C++17 的 string_view 接口。
 */

#ifndef GALAY_UTILS_BASE64_H
#define GALAY_UTILS_BASE64_H

#include <string>
#include <algorithm>
#include <cctype>
#include <stdexcept>

#if __cplusplus >= 201703L
#include <string_view>
#endif

namespace galay::utils
{
    /// 标准 Base64 和 URL 安全 Base64 字符集
    inline constexpr const char *base64_chars[2] = {
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "+/",

        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "-_"};

    /// 解码查找表，将 ASCII 值映射到 Base64 值，无效字符标记为 0xFF
    inline constexpr unsigned char decode_table[256] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3E, 0xFF, 0x3E, 0xFF, 0x3F,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
        0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F,
        0xFF, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };

    /**
     * @brief Base64 编解码工具类
     * @details 提供 Base64 编码和解码的静态方法，支持标准 Base64、URL 安全变体、
     *          PEM（64 字符换行）和 MIME（76 字符换行）格式。
     */
    class Base64Util
    {
    public:
        /**
         * @brief 对字符串进行 Base64 编码
         * @param s 待编码的字符串
         * @param url 是否使用 URL 安全字符集（- 和 _ 替换 + 和 /）
         * @return Base64 编码后的字符串
         */
        static std::string Base64Encode(std::string const &s, bool url = false);

        /**
         * @brief 以 PEM 格式编码（64 字符换行）
         * @param s 待编码的字符串
         * @return PEM 格式的 Base64 编码字符串
         */
        static std::string Base64EncodePem(std::string const &s);

        /**
         * @brief 以 MIME 格式编码（76 字符换行）
         * @param s 待编码的字符串
         * @return MIME 格式的 Base64 编码字符串
         */
        static std::string Base64EncodeMime(std::string const &s);

        /**
         * @brief 对 Base64 字符串进行解码
         * @param s 待解码的 Base64 字符串
         * @param remove_linebreaks 是否在解码前移除换行符
         * @return 解码后的字符串
         */
        static std::string Base64Decode(std::string const &s, bool remove_linebreaks = false);

        /**
         * @brief 对原始字节进行 Base64 编码
         * @param bytes_to_encode 待编码的字节数组指针
         * @param len 字节长度
         * @param url 是否使用 URL 安全字符集
         * @return Base64 编码后的字符串
         */
        static std::string Base64Encode(unsigned char const *, size_t len, bool url = false);

#if __cplusplus >= 201703L
        /**
         * @brief 对 string_view 进行 Base64 编码（C++17）
         * @param s 待编码的字符串视图
         * @param url 是否使用 URL 安全字符集
         * @return Base64 编码后的字符串
         */
        static std::string Base64EncodeView(std::string_view s, bool url = false);

        /**
         * @brief 以 PEM 格式编码 string_view（C++17）
         * @param s 待编码的字符串视图
         * @return PEM 格式的 Base64 编码字符串
         */
        static std::string Base64EncodePemView(std::string_view s);

        /**
         * @brief 以 MIME 格式编码 string_view（C++17）
         * @param s 待编码的字符串视图
         * @return MIME 格式的 Base64 编码字符串
         */
        static std::string Base64EncodeMimeView(std::string_view s);

        /**
         * @brief 对 string_view 进行 Base64 解码（C++17）
         * @param s 待解码的 Base64 字符串视图
         * @param remove_linebreaks 是否在解码前移除换行符
         * @return 解码后的字符串
         */
        static std::string Base64DecodeView(std::string_view s, bool remove_linebreaks = false);
#endif
    private:
        template <typename String>
        static std::string Decode(String const &encoded_string, bool remove_linebreaks)
        {
            //
            // Decode(…) is templated so that it can be used with String = const std::string&
            // or std::string_view (requires at least C++17)
            //

            if (encoded_string.empty())
                return std::string();

            if (remove_linebreaks)
            {

                std::string copy(encoded_string);

                copy.erase(std::remove_if(copy.begin(), copy.end(), [](unsigned char ch) {
                    return std::isspace(ch) != 0;
                }), copy.end());

                return Base64Decode(copy, false);
            }

            size_t length_of_string = encoded_string.length();
            if (length_of_string % 4 == 1) {
                throw std::runtime_error("Input is not valid base64-encoded data.");
            }
            size_t pos = 0;

            //
            // The approximate length (bytes) of the decoded string might be one or
            // two bytes smaller, depending on the amount of trailing equal signs
            // in the encoded string. This approximation is needed to reserve
            // enough space in the string to be returned.
            //
            size_t approx_length_of_decoded_string = length_of_string / 4 * 3;
            std::string ret;
            ret.reserve(approx_length_of_decoded_string);

            while (pos < length_of_string)
            {
                if (length_of_string - pos == 1) {
                    throw std::runtime_error("Input is not valid base64-encoded data.");
                }
                //
                // Iterate over encoded input string in chunks. The size of all
                // chunks except the last one is 4 bytes.
                //
                // The last chunk might be padded with equal signs or dots
                // in order to make it 4 bytes in size as well, but this
                // is not required as per RFC 2045.
                //
                // All chunks except the last one produce three output bytes.
                //
                // The last chunk produces at least one and up to three bytes.
                //

                size_t pos_of_char_1 = pos_of_char(encoded_string.at(pos + 1));

                //
                // Emit the first output byte that is produced in each chunk:
                //
                ret.push_back(static_cast<std::string::value_type>(((pos_of_char(encoded_string.at(pos + 0))) << 2) + ((pos_of_char_1 & 0x30) >> 4)));

                if ((pos + 2 < length_of_string) && // Check for data that is not padded with equal signs (which is allowed by RFC 2045)
                    encoded_string.at(pos + 2) != '=' &&
                    encoded_string.at(pos + 2) != '.' // accept URL-safe base 64 strings, too, so check for '.' also.
                )
                {
                    //
                    // Emit a chunk's second byte (which might not be produced in the last chunk).
                    //
                    unsigned int pos_of_char_2 = pos_of_char(encoded_string.at(pos + 2));
                    ret.push_back(static_cast<std::string::value_type>(((pos_of_char_1 & 0x0f) << 4) + ((pos_of_char_2 & 0x3c) >> 2)));

                    if ((pos + 3 < length_of_string) &&
                        encoded_string.at(pos + 3) != '=' &&
                        encoded_string.at(pos + 3) != '.')
                    {
                        //
                        // Emit a chunk's third byte (which might not be produced in the last chunk).
                        //
                        ret.push_back(static_cast<std::string::value_type>(((pos_of_char_2 & 0x03) << 6) + pos_of_char(encoded_string.at(pos + 3))));
                    }
                }

                pos += 4;
            }

            return ret;
        }

        static inline unsigned int pos_of_char(const unsigned char chr)
        {
            unsigned char value = decode_table[chr];
            if (value == 0xFF) {
                throw std::runtime_error("Input is not valid base64-encoded data.");
            }
            return value;
        }

        static std::string insert_linebreaks(std::string str, size_t distance)
        {
            if (str.empty() || distance == 0)
            {
                return str;
            }

            // Calculate the number of line breaks needed
            size_t num_breaks = (str.length() - 1) / distance;
            if (num_breaks == 0)
            {
                return str;
            }

            // Pre-allocate the result string with exact size
            std::string result;
            result.reserve(str.length() + num_breaks);

            // Copy chunks with line breaks
            size_t pos = 0;
            while (pos < str.length())
            {
                size_t chunk_size = std::min(distance, str.length() - pos);
                result.append(str, pos, chunk_size);
                pos += chunk_size;

                if (pos < str.length())
                {
                    result.push_back('\n');
                }
            }

            return result;
        }

        template <typename String, unsigned int line_length>
        static std::string encode_with_line_breaks(String s)
        {
            return insert_linebreaks(Encode(s, false), line_length);
        }

        template <typename String>
        static std::string encode_pem(String s)
        {
            return encode_with_line_breaks<String, 64>(s);
        }

        template <typename String>
        static std::string encode_mime(String s)
        {
            return encode_with_line_breaks<String, 76>(s);
        }

        template <typename String>
        static std::string Encode(String s, bool url)
        {
            return Base64Encode(reinterpret_cast<const unsigned char *>(s.data()), s.length(), url);
        }
    };

    // Implementation of Base64Encode functions
    inline std::string Base64Util::Base64Encode(unsigned char const *bytes_to_encode, size_t in_len, bool url)
    {
        std::string ret;
        int i = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];

        const char *base64_chars_selected = base64_chars[url ? 1 : 0];

        // Pre-calculate and reserve exact size needed
        size_t encoded_size = ((in_len + 2) / 3) * 4;
        ret.reserve(encoded_size);

        while (in_len--)
        {
            char_array_3[i++] = *(bytes_to_encode++);
            if (i == 3)
            {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;

                for (i = 0; i < 4; i++)
                    ret += base64_chars_selected[char_array_4[i]];
                i = 0;
            }
        }

        if (i)
        {
            for (int j = i; j < 3; j++)
                char_array_3[j] = '\0';

            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

            for (int j = 0; j < i + 1; j++)
                ret += base64_chars_selected[char_array_4[j]];

            while (i++ < 3)
                ret += '=';
        }

        return ret;
    }

    inline std::string Base64Util::Base64Encode(std::string const &s, bool url)
    {
        return Encode(s, url);
    }

    inline std::string Base64Util::Base64EncodePem(std::string const &s)
    {
        return encode_pem(s);
    }

    inline std::string Base64Util::Base64EncodeMime(std::string const &s)
    {
        return encode_mime(s);
    }

    inline std::string Base64Util::Base64Decode(std::string const &s, bool remove_linebreaks)
    {
        return Decode(s, remove_linebreaks);
    }

#if __cplusplus >= 201703L
    // String view implementations
    inline std::string Base64Util::Base64EncodeView(std::string_view s, bool url)
    {
        return Encode(s, url);
    }

    inline std::string Base64Util::Base64EncodePemView(std::string_view s)
    {
        return encode_pem(s);
    }

    inline std::string Base64Util::Base64EncodeMimeView(std::string_view s)
    {
        return encode_mime(s);
    }

    inline std::string Base64Util::Base64DecodeView(std::string_view s, bool remove_linebreaks)
    {
        return Decode(s, remove_linebreaks);
    }
#endif

}


#endif /* BASE64_H_C0CE2A47_D10E_42C9_A27C_C883944E704A */
