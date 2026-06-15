#ifndef GALAY_UTILS_STRING_HPP
#define GALAY_UTILS_STRING_HPP

#include "galay-utils/common/defn.hpp"
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
     */
    static std::vector<std::string> splitRespectQuotes(std::string_view str, char delimiter, char quote = '"') {
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
     */
    static std::string trimLeft(std::string_view str) {
        size_t start = 0;
        while (start < str.length() && std::isspace(static_cast<unsigned char>(str[start]))) {
            ++start;
        }
        return std::string(str.substr(start));
    }

    /**
     * @brief Trim whitespace from right
     */
    static std::string trimRight(std::string_view str) {
        size_t end = str.length();
        while (end > 0 && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
            --end;
        }
        return std::string(str.substr(0, end));
    }

    /**
     * @brief Convert string to lowercase
     */
    static std::string toLower(std::string_view str) {
        std::string result(str);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return result;
    }

    /**
     * @brief Convert string to uppercase
     */
    static std::string toUpper(std::string_view str) {
        std::string result(str);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        return result;
    }

    /**
     * @brief Check if string starts with prefix
     */
    static bool startsWith(std::string_view str, std::string_view prefix) {
        if (prefix.length() > str.length()) return false;
        return str.substr(0, prefix.length()) == prefix;
    }

    /**
     * @brief Check if string ends with suffix
     */
    static bool endsWith(std::string_view str, std::string_view suffix) {
        if (suffix.length() > str.length()) return false;
        return str.substr(str.length() - suffix.length()) == suffix;
    }

    /**
     * @brief Check if string contains substring
     */
    static bool contains(std::string_view str, std::string_view substr) {
        return str.find(substr) != std::string_view::npos;
    }

    /**
     * @brief Replace all occurrences of a substring
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
     */
    static std::string replaceFirst(std::string_view str, std::string_view from, std::string_view to) {
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
     */
    static size_t count(std::string_view str, char ch) {
        return std::count(str.begin(), str.end(), ch);
    }

    /**
     * @brief Count occurrences of a substring
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
     */
    static std::string toHex(const uint8_t* data, size_t len, bool uppercase = false) {
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
     */
    static std::vector<uint8_t> fromHex(std::string_view hex) {
        if (hex.empty() || (hex.length() % 2) != 0) {
            return {};
        }

        std::vector<uint8_t> result;
        result.reserve(hex.length() / 2);

        auto hexCharToValue = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        for (size_t i = 0; i + 1 < hex.length(); i += 2) {
            int high = hexCharToValue(hex[i]);
            int low = hexCharToValue(hex[i + 1]);
            if (high < 0 || low < 0) {
                return {};
            }
            result.push_back(static_cast<uint8_t>((high << 4) | low));
        }

        return result;
    }

    /**
     * @brief Convert bytes to visible hex string (with spaces)
     */
    static std::string toVisibleHex(const uint8_t* data, size_t len) {
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
     */
    static bool isInteger(std::string_view str) {
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
     */
    static bool isFloat(std::string_view str) {
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
     */
    static bool isBlank(std::string_view str) {
        for (char c : str) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Format string with printf-style arguments
     */
    static std::string format(const char* fmt) {
        if (fmt == nullptr) {
            return {};
        }
        return std::string(fmt);
    }

    /**
     * @brief Format string with printf-style arguments
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
     */
    template<typename T>
    static std::string toString(const T& value) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_STRING_HPP
