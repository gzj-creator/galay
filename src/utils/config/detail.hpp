/**
 * @file detail.hpp
 * @brief 解析器内部辅助函数
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供字符串修剪、转义处理、引号去除、逗号分割和内联注释去除等
 *          解析器共用的内部工具函数。
 */

#ifndef GALAY_UTILS_PARSER_DETAIL_HPP
#define GALAY_UTILS_PARSER_DETAIL_HPP

#include <cctype>
#include <string>
#include <vector>

namespace galay::utils::parser_detail {

/**
 * @brief 去除字符串两端空白字符
 * @param text 输入字符串
 * @return 修剪后的字符串
 */
inline std::string trim(const std::string& text) {
    size_t start = 0;
    size_t end = text.length();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(start, end - start);
}

/**
 * @brief 处理转义字符
 * @param text 输入字符串
 * @return 处理转义后的字符串
 */
inline std::string processEscapes(const std::string& text) {
    std::string result;
    result.reserve(text.length());

    for (size_t index = 0; index < text.length(); ++index) {
        if (text[index] == '\\' && index + 1 < text.length()) {
            switch (text[index + 1]) {
                case 'n': result += '\n'; ++index; break;
                case 't': result += '\t'; ++index; break;
                case 'r': result += '\r'; ++index; break;
                case '\\': result += '\\'; ++index; break;
                case '"': result += '"'; ++index; break;
                case '\'': result += '\''; ++index; break;
                default: result += text[index]; break;
            }
        } else {
            result += text[index];
        }
    }

    return result;
}

/**
 * @brief 判断字符串是否被引号包裹
 * @param text 输入字符串
 * @return 被引号包裹返回 true
 */
inline bool isQuoted(const std::string& text) {
    return text.length() >= 2 &&
        ((text.front() == '"' && text.back() == '"') ||
         (text.front() == '\'' && text.back() == '\''));
}

/**
 * @brief 去除字符串外层引号并处理转义
 * @param text 输入字符串
 * @return 去除引号后的字符串
 */
inline std::string unquote(const std::string& text) {
    if (!isQuoted(text)) {
        return text;
    }
    return processEscapes(text.substr(1, text.length() - 2));
}

/**
 * @brief 按逗号分割字符串，尊重引号区域
 * @param text 输入字符串
 * @return 分割后的字符串向量
 */
inline std::vector<std::string> splitCommaSeparated(const std::string& text) {
    if (trim(text).empty()) {
        return {};
    }

    std::vector<std::string> result;
    std::string item;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    for (char character : text) {
        if (escaped) {
            item += character;
            escaped = false;
            continue;
        }

        if (character == '\\') {
            item += character;
            escaped = true;
            continue;
        }

        if (character == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            item += character;
            continue;
        }

        if (character == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            item += character;
            continue;
        }

        if (character == ',' && !in_single_quote && !in_double_quote) {
            result.push_back(trim(item));
            item.clear();
            continue;
        }

        item += character;
    }

    result.push_back(trim(item));
    return result;
}

/**
 * @brief 去除内联注释（尊重引号区域）
 * @param text 输入字符串
 * @param comment_character 注释起始字符
 * @return 去除注释后的字符串
 */
inline std::string stripInlineComment(const std::string& text, char comment_character) {
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    for (size_t index = 0; index < text.length(); ++index) {
        char character = text[index];

        if (escaped) {
            escaped = false;
            continue;
        }

        if (character == '\\') {
            escaped = true;
            continue;
        }

        if (character == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }

        if (character == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }

        if (character == comment_character && !in_single_quote && !in_double_quote) {
            return trim(text.substr(0, index));
        }
    }

    return trim(text);
}

} // namespace galay::utils::parser_detail

#endif // GALAY_UTILS_PARSER_DETAIL_HPP
