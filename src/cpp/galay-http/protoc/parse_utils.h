/**
 * @file parse_utils.h
 * @brief HTTP 协议解析辅助工具函数
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供大小写转换、Header 值 token 匹配、数值解析、
 *          iovec 切片等底层解析工具，供内部模块使用。
 */

#ifndef GALAY_HTTP_PARSE_UTILS_H
#define GALAY_HTTP_PARSE_UTILS_H

#include "http_header.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <sys/uio.h>

namespace galay::http::detail
{

/**
 * @brief 将 ASCII 大写字母转换为小写
 * @param ch 输入字符
 * @return 转换后的字符
 */
inline char to_lower_ascii_char(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch + ('a' - 'A'));
    }
    return ch;
}

/**
 * @brief 将 ASCII 小写字母转换为大写
 * @param ch 输入字符
 * @return 转换后的字符
 */
inline char to_upper_ascii_char(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return static_cast<char>(ch - ('a' - 'A'));
    }
    return ch;
}

/**
 * @brief 忽略大小写比较两个 ASCII 字符串
 * @param lhs 左操作数
 * @param rhs 右操作数
 * @return 相等返回 true
 */
inline bool equals_ignore_case_ascii(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (size_t i = 0; i < lhs.size(); ++i) {
        if (to_lower_ascii_char(lhs[i]) != to_lower_ascii_char(rhs[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 检查 Header 值中是否包含指定 token（逗号分隔，忽略大小写）
 * @param value Header 值字符串
 * @param token 要查找的 token
 * @return 包含返回 true
 */
inline bool header_value_contains_token(std::string_view value, std::string_view token)
{
    if (value.empty() || token.empty()) {
        return false;
    }

    size_t start = 0;
    while (start < value.size()) {
        size_t end = value.find(',', start);
        if (end == std::string_view::npos) {
            end = value.size();
        }

        size_t left = start;
        while (left < end && std::isspace(static_cast<unsigned char>(value[left]))) {
            ++left;
        }
        size_t right = end;
        while (right > left && std::isspace(static_cast<unsigned char>(value[right - 1]))) {
            --right;
        }

        if (right > left && equals_ignore_case_ascii(value.substr(left, right - left), token)) {
            return true;
        }
        start = end + 1;
    }

    return false;
}

/**
 * @brief 宽松模式获取 Header 值指针
 * @param headers HeaderPair 对象
 * @param key 头部键名
 * @return 值指针，不存在时返回 nullptr
 */
inline const std::string* get_header_value_ptr_loose(const HeaderPair& headers, const std::string& key)
{
    return headers.get_value_ptr(key);
}

/**
 * @brief 严格解析字符串为 size_t（不允许多余字符）
 * @param input 输入字符串
 * @return 解析成功返回值，失败返回 std::nullopt
 */
inline std::optional<size_t> parse_size_t_strict(std::string_view input)
{
    size_t begin = 0;
    size_t end = input.size();

    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    if (begin == end) {
        return std::nullopt;
    }

    size_t value = 0;
    const char* first = input.data() + begin;
    const char* last = input.data() + end;
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

/**
 * @brief 跳过前 N 个字节，切片 iovec 数组
 * @param iovecs 原始 iovec 数组
 * @param skip_bytes 要跳过的字节数
 * @return 切片后的 iovec 数组
 */
inline std::vector<iovec> slice_iovecs(const std::vector<iovec>& iovecs, size_t skip_bytes)
{
    std::vector<iovec> sliced;
    sliced.reserve(iovecs.size());

    size_t skip = skip_bytes;
    for (const auto& iov : iovecs) {
        if (skip >= iov.iov_len) {
            skip -= iov.iov_len;
            continue;
        }

        const char* base = static_cast<const char*>(iov.iov_base);
        iovec part{};
        part.iov_base = const_cast<char*>(base + skip);
        part.iov_len = iov.iov_len - skip;
        sliced.push_back(part);
        skip = 0;
    }

    return sliced;
}

/**
 * @brief 宽松模式移除 Header（尝试多种大小写变体）
 * @param headers HeaderPair 对象
 * @param key 头部键名
 * @return 无返回值
 * @details 依次尝试原始键名、全小写、全大写、首字母大写等形式进行移除
 */
inline void remove_header_pair_loose(HeaderPair& headers, const std::string& key)
{
    headers.remove_header_pair(key);

    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char c) { return to_lower_ascii_char(c); });
    if (lower != key) {
        headers.remove_header_pair(lower);
    }

    std::string upper = key;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return to_upper_ascii_char(c); });
    if (upper != key && upper != lower) {
        headers.remove_header_pair(upper);
    }

    std::string canonical = lower;
    bool word_start = true;
    for (char& ch : canonical) {
        ch = word_start ? to_upper_ascii_char(ch) : to_lower_ascii_char(ch);
        word_start = (ch == '-');
    }
    if (canonical != key && canonical != lower && canonical != upper) {
        headers.remove_header_pair(canonical);
    }
}

} // namespace galay::http::detail

#endif // GALAY_HTTP_PARSE_UTILS_H
