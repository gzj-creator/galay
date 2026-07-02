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
         * @return 解码后的字符串，输入无效时返回空字符串
         */
        static std::string Base64Decode(std::string const &s, bool remove_linebreaks = false);

        /**
         * @brief 检查 Base64 字符串是否可解码
         * @param s 待检查的 Base64 字符串
         * @param remove_linebreaks 是否在检查前移除换行符
         * @return 输入可解码时返回 true
         */
        static bool Base64CanDecode(std::string const &s, bool remove_linebreaks = false);

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
         * @return 解码后的字符串，输入无效时返回空字符串
         */
        static std::string Base64DecodeView(std::string_view s, bool remove_linebreaks = false);

        /**
         * @brief 检查 string_view 是否为可解码的 Base64 输入（C++17）
         * @param s 待检查的 Base64 字符串视图
         * @param remove_linebreaks 是否在检查前移除换行符
         * @return 输入可解码时返回 true
         */
        static bool Base64CanDecodeView(std::string_view s, bool remove_linebreaks = false);
#endif
    private:
        static constexpr unsigned int invalid_char = 0xffU;

        static bool is_decode_whitespace(unsigned char ch)
        {
            return std::isspace(ch) != 0;
        }

        static bool can_decode_chunk(const unsigned char char_array_4[4], size_t chars_in_chunk)
        {
            if (chars_in_chunk == 0) {
                return true;
            }
            if (chars_in_chunk == 1) {
                return false;
            }

            const unsigned int first = pos_of_char(char_array_4[0]);
            const unsigned int second = pos_of_char(char_array_4[1]);
            if (first == invalid_char || second == invalid_char) {
                return false;
            }

            if (chars_in_chunk > 2 &&
                char_array_4[2] != '=' &&
                char_array_4[2] != '.') {
                const unsigned int third = pos_of_char(char_array_4[2]);
                if (third == invalid_char) {
                    return false;
                }

                if (chars_in_chunk > 3 &&
                    char_array_4[3] != '=' &&
                    char_array_4[3] != '.') {
                    const unsigned int fourth = pos_of_char(char_array_4[3]);
                    if (fourth == invalid_char) {
                        return false;
                    }
                }
            }

            return true;
        }

        static bool append_decode_chunk(const unsigned char char_array_4[4], size_t chars_in_chunk, std::string &ret)
        {
            if (chars_in_chunk == 0) {
                return true;
            }
            if (chars_in_chunk == 1) {
                return false;
            }

            const unsigned int pos_of_char_0 = pos_of_char(char_array_4[0]);
            const unsigned int pos_of_char_1 = pos_of_char(char_array_4[1]);
            if (pos_of_char_0 == invalid_char || pos_of_char_1 == invalid_char) {
                return false;
            }

            unsigned int pos_of_char_2 = 0;
            const bool emit_second = chars_in_chunk > 2 &&
                char_array_4[2] != '=' &&
                char_array_4[2] != '.';
            if (emit_second) {
                pos_of_char_2 = pos_of_char(char_array_4[2]);
                if (pos_of_char_2 == invalid_char) {
                    return false;
                }
            }

            unsigned int pos_of_char_3 = 0;
            const bool emit_third = emit_second &&
                chars_in_chunk > 3 &&
                char_array_4[3] != '=' &&
                char_array_4[3] != '.';
            if (emit_third) {
                pos_of_char_3 = pos_of_char(char_array_4[3]);
                if (pos_of_char_3 == invalid_char) {
                    return false;
                }
            }

            ret.push_back(static_cast<std::string::value_type>((pos_of_char_0 << 2) + ((pos_of_char_1 & 0x30) >> 4)));
            if (emit_second) {
                ret.push_back(static_cast<std::string::value_type>(((pos_of_char_1 & 0x0f) << 4) + ((pos_of_char_2 & 0x3c) >> 2)));
            }
            if (emit_third) {
                ret.push_back(static_cast<std::string::value_type>(((pos_of_char_2 & 0x03) << 6) + pos_of_char_3));
            }

            return true;
        }

        template <typename String>
        static std::string Decode(String const &encoded_string, bool remove_linebreaks)
        {
            //
            // Decode(…) is templated so that it can be used with String = const std::string&
            // or std::string_view (requires at least C++17)
            //

            if (encoded_string.empty())
                return std::string();

            //
            // The approximate length (bytes) of the decoded string might be one or
            // two bytes smaller, depending on the amount of trailing equal signs
            // in the encoded string. This approximation is needed to reserve
            // enough space in the string to be returned.
            //
            size_t approx_length_of_decoded_string = encoded_string.length() / 4 * 3;
            std::string ret;
            ret.reserve(approx_length_of_decoded_string);

            if (!remove_linebreaks) {
                for (size_t pos = 0; pos < encoded_string.length(); pos += 4) {
                    unsigned char char_array_4[4] = {};
                    const size_t chars_in_chunk = std::min<size_t>(4, encoded_string.length() - pos);
                    for (size_t i = 0; i < chars_in_chunk; ++i) {
                        char_array_4[i] = static_cast<unsigned char>(encoded_string.at(pos + i));
                    }
                    if (!append_decode_chunk(char_array_4, chars_in_chunk, ret)) {
                        return std::string();
                    }
                }

                return ret;
            }

            unsigned char char_array_4[4] = {};
            size_t chars_in_chunk = 0;
            for (size_t pos = 0; pos < encoded_string.length(); ++pos) {
                const auto ch = static_cast<unsigned char>(encoded_string.at(pos));
                if (is_decode_whitespace(ch)) {
                    continue;
                }

                char_array_4[chars_in_chunk++] = ch;
                if (chars_in_chunk == 4) {
                    if (!append_decode_chunk(char_array_4, chars_in_chunk, ret)) {
                        return std::string();
                    }
                    chars_in_chunk = 0;
                }
            }

            if (!append_decode_chunk(char_array_4, chars_in_chunk, ret)) {
                return std::string();
            }

            return ret;
        }

        static inline unsigned int pos_of_char(const unsigned char chr)
        {
            unsigned char value = decode_table[chr];
            return value == 0xFF ? invalid_char : value;
        }

        template <typename String>
        static bool CanDecode(String const &encoded_string, bool remove_linebreaks)
        {
            if (encoded_string.empty()) {
                return true;
            }

            if (!remove_linebreaks) {
                const size_t length_of_string = encoded_string.length();
                if (length_of_string % 4 == 1) {
                    return false;
                }

                for (size_t pos = 0; pos < length_of_string; pos += 4) {
                    unsigned char char_array_4[4] = {};
                    const size_t chars_in_chunk = std::min<size_t>(4, length_of_string - pos);
                    for (size_t i = 0; i < chars_in_chunk; ++i) {
                        char_array_4[i] = static_cast<unsigned char>(encoded_string.at(pos + i));
                    }
                    if (!can_decode_chunk(char_array_4, chars_in_chunk)) {
                        return false;
                    }
                }

                return true;
            }

            unsigned char char_array_4[4] = {};
            size_t chars_in_chunk = 0;
            for (size_t pos = 0; pos < encoded_string.length(); ++pos) {
                const auto ch = static_cast<unsigned char>(encoded_string.at(pos));
                if (is_decode_whitespace(ch)) {
                    continue;
                }

                char_array_4[chars_in_chunk++] = ch;
                if (chars_in_chunk == 4) {
                    if (!can_decode_chunk(char_array_4, chars_in_chunk)) {
                        return false;
                    }
                    chars_in_chunk = 0;
                }
            }

            return can_decode_chunk(char_array_4, chars_in_chunk);
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
        const char *base64_chars_selected = base64_chars[url ? 1 : 0];

        const size_t encoded_size = ((in_len + 2) / 3) * 4;
        std::string ret(encoded_size, '\0');
        size_t output_pos = 0;

        while (in_len >= 3) {
            const unsigned char first = bytes_to_encode[0];
            const unsigned char second = bytes_to_encode[1];
            const unsigned char third = bytes_to_encode[2];

            ret[output_pos++] = base64_chars_selected[(first & 0xfc) >> 2];
            ret[output_pos++] = base64_chars_selected[((first & 0x03) << 4) + ((second & 0xf0) >> 4)];
            ret[output_pos++] = base64_chars_selected[((second & 0x0f) << 2) + ((third & 0xc0) >> 6)];
            ret[output_pos++] = base64_chars_selected[third & 0x3f];

            bytes_to_encode += 3;
            in_len -= 3;
        }

        if (in_len == 1) {
            const unsigned char first = bytes_to_encode[0];
            ret[output_pos++] = base64_chars_selected[(first & 0xfc) >> 2];
            ret[output_pos++] = base64_chars_selected[(first & 0x03) << 4];
            ret[output_pos++] = '=';
            ret[output_pos++] = '=';
        } else if (in_len == 2) {
            const unsigned char first = bytes_to_encode[0];
            const unsigned char second = bytes_to_encode[1];
            ret[output_pos++] = base64_chars_selected[(first & 0xfc) >> 2];
            ret[output_pos++] = base64_chars_selected[((first & 0x03) << 4) + ((second & 0xf0) >> 4)];
            ret[output_pos++] = base64_chars_selected[(second & 0x0f) << 2];
            ret[output_pos++] = '=';
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

    inline bool Base64Util::Base64CanDecode(std::string const &s, bool remove_linebreaks)
    {
        return CanDecode(s, remove_linebreaks);
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

    inline bool Base64Util::Base64CanDecodeView(std::string_view s, bool remove_linebreaks)
    {
        return CanDecode(s, remove_linebreaks);
    }
#endif

}


#endif /* BASE64_H_C0CE2A47_D10E_42C9_A27C_C883944E704A */
