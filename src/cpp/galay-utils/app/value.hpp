/**
 * @file value.hpp
 * @brief 命令行取值的无异常类型转换
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 通过 `std::from_chars` 完成数值转换，失败以 `std::expected` 返回原因，
 *          不使用异常。支持 bool、整数、浮点、`std::string` 与 `std::string_view`。
 */

#ifndef GALAY_UTILS_APP_VALUE_HPP
#define GALAY_UTILS_APP_VALUE_HPP

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>

namespace galay::utils {

namespace detail {

/**
 * @brief 浮点解析回退实现
 * @details 部分标准库尚未提供浮点 `std::from_chars`，此处用 `strtod` 家族替代，
 *          通过 `errno` 与结束指针判断失败，不使用异常。
 */
template<typename T>
std::expected<T, std::string> parseFloating(std::string_view text) {
    const std::string buffer(text);
    char* end = nullptr;
    errno = 0;

    T out{};
    if constexpr (std::is_same_v<T, float>) {
        out = std::strtof(buffer.c_str(), &end);
    } else if constexpr (std::is_same_v<T, long double>) {
        out = std::strtold(buffer.c_str(), &end);
    } else {
        out = static_cast<T>(std::strtod(buffer.c_str(), &end));
    }

    if (end == buffer.c_str()) {
        return std::unexpected(std::string("expected float"));
    }
    if (*end != '\0') {
        return std::unexpected(std::string("trailing characters"));
    }
    if (errno == ERANGE) {
        return std::unexpected(std::string("out of range"));
    }
    return out;
}

} // namespace detail

/**
 * @brief 命令行标量取值转换器
 * @tparam T 目标类型
 */
template<typename T>
struct CliValue {
    static_assert(std::is_arithmetic_v<T>, "unsupported cli value type");

    /// 类型名，用于帮助输出
    static constexpr std::string_view typeName() noexcept {
        if constexpr (std::is_floating_point_v<T>) {
            return "float";
        } else if constexpr (std::is_unsigned_v<T>) {
            return "uint";
        } else {
            return "int";
        }
    }

    /**
     * @brief 解析字符串为目标类型
     * @param text 输入文本
     * @return 成功返回值，失败返回原因说明
     */
    static std::expected<T, std::string> parse(std::string_view text) {
        if (text.empty()) {
            return std::unexpected(std::string("empty value"));
        }
        if constexpr (std::is_floating_point_v<T>) {
            return detail::parseFloating<T>(text);
        } else {
            T out{};
            const char* first = text.data();
            const char* last = text.data() + text.size();
            auto result = std::from_chars(first, last, out);
            if (result.ec != std::errc{}) {
                return std::unexpected(result.ec == std::errc::result_out_of_range
                                           ? std::string("out of range")
                                           : std::string("expected ") + std::string(typeName()));
            }
            if (result.ptr != last) {
                return std::unexpected(std::string("trailing characters"));
            }
            return out;
        }
    }

    /// 转为可读字符串，用于帮助输出
    static std::string toString(const T& value) { return std::to_string(value); }
};

/// bool 转换器：接受 true/false、1/0、yes/no、on/off
template<>
struct CliValue<bool> {
    static constexpr std::string_view typeName() noexcept { return "bool"; }

    static std::expected<bool, std::string> parse(std::string_view text) {
        if (text == "true" || text == "1" || text == "yes" || text == "on") {
            return true;
        }
        if (text == "false" || text == "0" || text == "no" || text == "off") {
            return false;
        }
        return std::unexpected(std::string("expected true/false"));
    }

    static std::string toString(bool value) { return value ? "true" : "false"; }
};

/// std::string 转换器：原样透传
template<>
struct CliValue<std::string> {
    static constexpr std::string_view typeName() noexcept { return "string"; }

    static std::expected<std::string, std::string> parse(std::string_view text) {
        return std::string(text);
    }

    static std::string toString(const std::string& value) { return value; }
};

} // namespace galay::utils

#endif // GALAY_UTILS_APP_VALUE_HPP
