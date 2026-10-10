#ifndef GALAY_UTILS_STRING_HPP
#define GALAY_UTILS_STRING_HPP

#include "../common/defn.hpp"
#include <vector>
#include <string>
#include <string_view>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <iomanip>
#include <cstdlib>
#include <type_traits>

namespace galay::utils {

/**
 * @file string.hpp
 * @brief 字符串工具类
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供常用的字符串操作，包括分割、连接、修剪、大小写转换、
 *          前后缀检查、替换、十六进制转换、类型判断和格式化等。
 */

/**
 * @brief 字符串工具类
 * @details 提供静态方法进行常见字符串操作。
 */
class StringUtils {
public:
    /**
     * @brief Split string by character delimiter
     * @param str 待处理字符串
     * @param delimiter 分隔符
     * @return 处理后的 std::vector<std::string> 结果
     */
    static std::vector<std::string> split(std::string_view str, char delimiter) {
        std::vector<std::string> result;
        if (str.empty()) return result;

        size_t start = 0;
        size_t end = str.find(delimiter);

        while (end != std::string_view::npos) {
            result.emplace_back(str.substr(start, end - start));
            start = end + 1;
            end = str.find(delimiter, start);
        }

        // 总是添加最后的子串
        result.emplace_back(str.substr(start));

        return result;
    }

    /**
     * @brief Split string by string delimiter
     * @param str 待处理字符串
     * @param delimiter 分隔符
     * @return 处理后的 std::vector<std::string> 结果
     */
    static std::vector<std::string> split(std::string_view str, std::string_view delimiter) {
        std::vector<std::string> result;
        if (str.empty()) return result;

        if (delimiter.empty()) {
            result.emplace_back(str);
            return result;
        }

        size_t start = 0;
        size_t end = str.find(delimiter);

        while (end != std::string_view::npos) {
            result.emplace_back(str.substr(start, end - start));
            start = end + delimiter.length();
            end = str.find(delimiter, start);
        }

        // 总是添加最后的子串
        result.emplace_back(str.substr(start));

        return result;
    }

    /**
     * @brief Split string by character, respecting quoted sections
     * @param str 待处理字符串
     * @param delimiter 分隔符
     * @param quote 引用字符
     * @return 处理后的 std::vector<std::string> 结果
     */
    static std::vector<std::string> split_respect_quotes(std::string_view str, char delimiter, char quote = '"') {
        std::vector<std::string> result;
        std::string current;
        bool inQuotes = false;

        for (size_t i = 0; i < str.length(); ++i) {
            char ch = str[i];

            if (ch == quote) {
                inQuotes = !inQuotes;
                current += ch;
            } else if (ch == delimiter && !inQuotes) {
                result.push_back(std::move(current));
                current.clear();
            } else {
                current += ch;
            }
        }

        if (!current.empty() || (!str.empty() && str.back() == delimiter)) {
            result.push_back(std::move(current));
        }

        return result;
    }

    /**
     * @brief Join strings with delimiter
     * @param parts 字符串片段集合
     * @param delimiter 分隔符
     * @return 处理后的 std::string 结果
     */
    static std::string join(const std::vector<std::string>& parts, std::string_view delimiter) {
        if (parts.empty()) return "";

        std::string result = parts[0];
        for (size_t i = 1; i < parts.size(); ++i) {
            result += delimiter;
            result += parts[i];
        }
        return result;
    }

    /**
     * @brief Trim whitespace from both ends
     * @param str 待处理字符串
     * @return 按函数说明处理后的字符串
     */
    static std::string trim(std::string_view str) {
        size_t start = 0;
        size_t end = str.length();

        while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
            ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
            --end;
        }

        return std::string(str.substr(start, end - start));
    }

    /**
     * @brief Trim whitespace from left
     * @param str 待处理字符串
     * @return 按函数说明处理后的字符串
     */
    static std::string trim_left(std::string_view str) {
        size_t start = 0;
        while (start < str.length() && std::isspace(static_cast<unsigned char>(str[start]))) {
            ++start;
        }
        return std::string(str.substr(start));
    }

    /**
     * @brief Trim whitespace from right
     * @param str 待处理字符串
     * @return 按函数说明处理后的字符串
     */
    static std::string trim_right(std::string_view str) {
        size_t end = str.length();
        while (end > 0 && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
            --end;
        }
        return std::string(str.substr(0, end));
    }

    /**
     * @brief Convert string to lowercase
     * @param str 待处理字符串
     * @return 按函数说明处理后的字符串
     */
    static std::string to_lower(std::string_view str) {
        std::string result(str);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return result;
    }

    /**
     * @brief Convert string to uppercase
     * @param str 待处理字符串
     * @return 按函数说明处理后的字符串
     */
    static std::string to_upper(std::string_view str) {
        std::string result(str);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        return result;
    }

    /**
     * @brief Check if string starts with prefix
     * @param str 待处理字符串
     * @param prefix 前缀
     * @return 匹配指定前缀时返回 true，否则返回 false
     */
    static bool starts_with(std::string_view str, std::string_view prefix) {
        if (prefix.length() > str.length()) return false;
        return str.substr(0, prefix.length()) == prefix;
    }

    /**
     * @brief Check if string ends with suffix
     * @param str 待处理字符串
     * @param suffix 后缀
     * @return 匹配指定后缀时返回 true，否则返回 false
     */
    static bool ends_with(std::string_view str, std::string_view suffix) {
        if (suffix.length() > str.length()) return false;
        return str.substr(str.length() - suffix.length()) == suffix;
    }

    /**
     * @brief Check if string contains substring
     * @param str 待处理字符串
     * @param substr 子字符串
     * @return 包含指定内容时返回 true，否则返回 false
     */
    static bool contains(std::string_view str, std::string_view substr) {
        return str.find(substr) != std::string_view::npos;
    }

    /**
     * @brief Replace all occurrences of a substring
     * @param str 待处理字符串
     * @param from 待替换子字符串
     * @param to 替换文本
     * @return 按函数说明处理后的字符串
     */
    static std::string replace(std::string_view str, std::string_view from, std::string_view to) {
        if (from.empty()) return std::string(str);

        std::string result;
        result.reserve(str.length());

        size_t start = 0;
        size_t pos = str.find(from);

        while (pos != std::string_view::npos) {
            result.append(str.substr(start, pos - start));
            result.append(to);
            start = pos + from.length();
            pos = str.find(from, start);
        }

        result.append(str.substr(start));
        return result;
    }

    /**
     * @brief Replace first occurrence of a substring
     * @param str 待处理字符串
     * @param from 待替换子字符串
     * @param to 替换文本
     * @return 按函数说明处理后的字符串
     */
    static std::string replace_first(std::string_view str, std::string_view from, std::string_view to) {
        if (from.empty()) return std::string(str);

        size_t pos = str.find(from);
        if (pos == std::string_view::npos) {
            return std::string(str);
        }

        std::string result;
        result.reserve(str.length() - from.length() + to.length());
        result.append(str.substr(0, pos));
        result.append(to);
        result.append(str.substr(pos + from.length()));
        return result;
    }

    /**
     * @brief Count occurrences of a character
     * @param str 待处理字符串
     * @param ch 字符
     * @return 对应的大小或数量
     */
    static size_t count(std::string_view str, char ch) {
        return std::count(str.begin(), str.end(), ch);
    }

    /**
     * @brief Count occurrences of a substring
     * @param str 待处理字符串
     * @param substr 子字符串
     * @return 对应的大小或数量
     */
    static size_t count(std::string_view str, std::string_view substr) {
        if (substr.empty()) return 0;

        size_t count = 0;
        size_t pos = 0;

        while ((pos = str.find(substr, pos)) != std::string_view::npos) {
            ++count;
            pos += substr.length();
        }

        return count;
    }

    /**
     * @brief Convert bytes to hex string
     * @param data 输入数据
     * @param len 数据字节数
     * @param uppercase 是否使用大写十六进制字符
     * @return 按函数说明处理后的字符串
     */
    static std::string to_hex(const uint8_t* data, size_t len, bool uppercase = false) {
        if (data == nullptr) {
            return {};
        }

        static const char* lowerHex = "0123456789abcdef";
        static const char* upperHex = "0123456789ABCDEF";
        const char* hexChars = uppercase ? upperHex : lowerHex;

        std::string result;
        result.reserve(len * 2);

        for (size_t i = 0; i < len; ++i) {
            result += hexChars[(data[i] >> 4) & 0x0F];
            result += hexChars[data[i] & 0x0F];
        }

        return result;
    }

    /**
     * @brief Convert hex string to bytes
     * @param hex 十六进制字符串
     * @return 处理后的 std::vector<uint8_t> 结果
     */
    static std::vector<uint8_t> from_hex(std::string_view hex) {
        if (hex.empty() || (hex.length() % 2) != 0) {
            return {};
        }

        std::vector<uint8_t> result;
        result.reserve(hex.length() / 2);

        auto hex_char_to_value = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        for (size_t i = 0; i + 1 < hex.length(); i += 2) {
            int high = hex_char_to_value(hex[i]);
            int low = hex_char_to_value(hex[i + 1]);
            if (high < 0 || low < 0) {
                return {};
            }
            result.push_back(static_cast<uint8_t>((high << 4) | low));
        }

        return result;
    }

    /**
     * @brief Convert bytes to visible hex string (with spaces)
     * @param data 输入数据
     * @param len 数据字节数
     * @return 按函数说明处理后的字符串
     */
    static std::string to_visible_hex(const uint8_t* data, size_t len) {
        if (data == nullptr) {
            return {};
        }

        static const char* hexChars = "0123456789ABCDEF";

        std::string result;
        result.reserve(len * 3);

        for (size_t i = 0; i < len; ++i) {
            if (i > 0) result += ' ';
            result += hexChars[(data[i] >> 4) & 0x0F];
            result += hexChars[data[i] & 0x0F];
        }

        return result;
    }

    /**
     * @brief Check if string is a valid integer
     * @param str 待处理字符串
     * @return 字符串表示有效整数时返回 true，否则返回 false
     */
    static bool is_integer(std::string_view str) {
        if (str.empty()) return false;

        size_t start = 0;
        if (str[0] == '+' || str[0] == '-') {
            start = 1;
            if (str.length() == 1) return false;
        }

        for (size_t i = start; i < str.length(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(str[i]))) {
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Check if string is a valid floating point number
     * @param str 待处理字符串
     * @return 字符串表示有效浮点数时返回 true，否则返回 false
     */
    static bool is_float(std::string_view str) {
        if (str.empty()) return false;

        size_t start = 0;
        if (str[0] == '+' || str[0] == '-') {
            start = 1;
            if (str.length() == 1) return false;
        }

        bool hasDecimal = false;
        bool hasExponent = false;
        bool hasDigit = false;

        for (size_t i = start; i < str.length(); ++i) {
            char c = str[i];

            if (std::isdigit(static_cast<unsigned char>(c))) {
                hasDigit = true;
            } else if (c == '.') {
                if (hasDecimal || hasExponent) return false;
                hasDecimal = true;
            } else if (c == 'e' || c == 'E') {
                if (hasExponent || !hasDigit) return false;
                hasExponent = true;
                hasDigit = false;
                if (i + 1 < str.length() && (str[i + 1] == '+' || str[i + 1] == '-')) {
                    ++i;
                }
            } else {
                return false;
            }
        }

        return hasDigit;
    }

    /**
     * @brief Check if string is empty or contains only whitespace
     * @param str 待处理字符串
     * @return 字符串为空或只含空白字符时返回 true，否则返回 false
     */
    static bool is_blank(std::string_view str) {
        for (char c : str) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Format string with printf-style arguments
     * @param fmt 格式字符串
     * @return 按函数说明处理后的字符串
     */
    static std::string format(const char* fmt) {
        if (fmt == nullptr) {
            return {};
        }
        return std::string(fmt);
    }

    /**
     * @brief Format string with printf-style arguments
     * @param fmt 格式字符串
     * @param args 调用参数包
     * @return 按函数说明处理后的字符串
     */
    template<typename... Args>
    static std::string format(const char* fmt, Args&&... args) {
        if (fmt == nullptr) {
            return {};
        }

        int size = std::snprintf(nullptr, 0, fmt, std::forward<Args>(args)...);
        if (size <= 0) return "";
        std::string result(size + 1, '\0');
        std::snprintf(result.data(), result.size(), fmt, std::forward<Args>(args)...);
        result.resize(size);
        return result;
    }

    /**
     * @brief Parse string to type T
     * @param str 待处理字符串
     * @param defaultValue 解析失败时使用的默认值
     * @return 解析出的 T 值；解析失败时返回 defaultValue
     */
    template<typename T>
    static T parse(std::string_view str, T defaultValue = T{}) {
        const std::string text = trim(str);
        if (text.empty()) {
            return defaultValue;
        }

        if constexpr (std::is_floating_point_v<T>) {
            char* end = nullptr;
            errno = 0;
            const long double value = std::strtold(text.c_str(), &end);
            if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
                return defaultValue;
            }
            return static_cast<T>(value);
        }

        std::istringstream iss{text};
        T value;
        if (iss >> value) {
            iss >> std::ws;
            if (!iss.eof()) {
                return defaultValue;
            }
            return value;
        }
        return defaultValue;
    }

    /**
     * @brief Convert value to string
     * @param value 待设置或处理的值
     * @return 按函数说明处理后的字符串
     */
    template<typename T>
    static std::string to_string(const T& value) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_STRING_HPP
