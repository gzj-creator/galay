/**
 * @file toml.hpp
 * @brief TOML 配置文件解析器
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 轻量级 TOML 解析器，支持键值对、[section] 头、点分键名、
 *          字符串、布尔值、数字和数组。不实现 TOML 的数组表、
 *          多行字符串、日期等高级特性。
 */

#ifndef GALAY_UTILS_PARSER_TOML_HPP
#define GALAY_UTILS_PARSER_TOML_HPP

#include "detail.hpp"
#include "parser_base.hpp"
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace galay::utils {

/**
 * @brief 轻量级 TOML 配置文件解析器
 * @details 支持键值对、[section] 头、点分键名、字符串、布尔值、数字和数组。
 *          不支持 TOML 的数组表、多行字符串、日期等高级特性。
 */
class TomlParser : public ParserBase {
public:
    TomlParser() = default;
    TomlParser(TomlParser&&) noexcept = default;
    TomlParser& operator=(TomlParser&&) noexcept = default;

    /**
     * @brief 显式克隆 TOML 解析状态
     * @return 独立 TomlParser 副本
     */
    [[nodiscard]] TomlParser clone() const {
        TomlParser copy;
        copy.m_values = m_values;
        copy.m_arrays = m_arrays;
        copy.m_sections = m_sections;
        copy.m_last_error = m_last_error;
        return copy;
    }

    bool parseFile(const std::string& path) override {
        return parseFileContent(path);
    }

    bool parseString(const std::string& content) override {
        m_values.clear();
        m_arrays.clear();
        m_sections.clear();
        m_last_error.clear();
        std::string current_section;
        std::string pending_array_key;
        std::string pending_array_section;
        std::string pending_array_value;
        int pending_array_line = 0;
        bool has_pending_array = false;

        std::istringstream input(content);
        std::string line;
        int line_num = 0;

        while (std::getline(input, line)) {
            ++line_num;
            line = stripTomlInlineComment(line);

            if (has_pending_array) {
                if (!line.empty()) {
                    pending_array_value += " ";
                    pending_array_value += line;
                    if (hasClosedArrayValue(pending_array_value)) {
                        if (!storeValue(pending_array_section, pending_array_key, pending_array_value, pending_array_line)) {
                            return false;
                        }
                        has_pending_array = false;
                        pending_array_key.clear();
                        pending_array_section.clear();
                        pending_array_value.clear();
                        pending_array_line = 0;
                    }
                }
                continue;
            }

            if (line.empty()) {
                continue;
            }

            if (line.front() == '[' && line.back() == ']') {
                if (line.length() < 3 || line[1] == '[' || line[line.length() - 2] == ']') {
                    m_last_error = "Unsupported TOML table line " + std::to_string(line_num) + ": " + line;
                    return false;
                }
                current_section = parser_detail::trim(line.substr(1, line.length() - 2));
                if (!isValidKey(current_section)) {
                    m_last_error = "Invalid TOML section at line " + std::to_string(line_num);
                    return false;
                }
                if (m_sections.find(current_section) != m_sections.end()) {
                    m_last_error = "Duplicate TOML section at line " + std::to_string(line_num) + ": " + current_section;
                    return false;
                }
                if (hasKeyConflict(current_section)) {
                    m_last_error = "TOML section conflicts with key at line " + std::to_string(line_num) + ": " + current_section;
                    return false;
                }
                m_sections.insert(current_section);
                continue;
            }

            auto equal_pos = line.find('=');
            if (equal_pos == std::string::npos) {
                m_last_error = "Invalid TOML line " + std::to_string(line_num) + ": " + line;
                return false;
            }

            std::string key = parser_detail::trim(line.substr(0, equal_pos));
            std::string raw_value = parser_detail::trim(line.substr(equal_pos + 1));
            if (key.empty()) {
                m_last_error = "Empty TOML key at line " + std::to_string(line_num);
                return false;
            }
            if (!isValidKey(key)) {
                m_last_error = "Invalid TOML key at line " + std::to_string(line_num) + ": " + key;
                return false;
            }

            if (!raw_value.empty() && raw_value.front() == '[' && !hasClosedArrayValue(raw_value)) {
                pending_array_key = key;
                pending_array_section = current_section;
                pending_array_value = raw_value;
                pending_array_line = line_num;
                has_pending_array = true;
                continue;
            }

            if (!storeValue(current_section, key, raw_value, line_num)) {
                return false;
            }
        }

        if (has_pending_array) {
            m_last_error = "Invalid TOML array at line " + std::to_string(pending_array_line);
            return false;
        }

        return true;
    }

    std::optional<std::string> getValue(const std::string& key) const override {
        auto iter = m_values.find(key);
        if (iter != m_values.end()) {
            return iter->second;
        }
        return std::nullopt;
    }

    bool hasKey(const std::string& key) const override {
        return m_values.find(key) != m_values.end();
    }

    std::vector<std::string> getKeys() const override {
        std::vector<std::string> keys;
        keys.reserve(m_values.size());
        for (const auto& entry : m_values) {
            keys.push_back(entry.first);
        }
        return keys;
    }

    std::vector<std::string> getArray(const std::string& key) const {
        auto array_iter = m_arrays.find(key);
        if (array_iter != m_arrays.end()) {
            return array_iter->second;
        }

        auto value = getValue(key);
        if (!value) {
            return {};
        }
        return parser_detail::splitCommaSeparated(*value);
    }

private:
    TomlParser(const TomlParser&) = delete;
    TomlParser& operator=(const TomlParser&) = delete;

    bool storeValue(const std::string& current_section, const std::string& key, const std::string& raw_value, int line_num) {
        std::vector<std::string> array_items;
        bool is_array = !raw_value.empty() && raw_value.front() == '[';
        std::string value = normalizeValue(raw_value, line_num, is_array ? &array_items : nullptr);
        if (!m_last_error.empty()) {
            return false;
        }

        std::string full_key = current_section.empty() ? key : current_section + "." + key;
        if (m_values.find(full_key) != m_values.end()) {
            m_last_error = "Duplicate TOML key at line " + std::to_string(line_num) + ": " + full_key;
            return false;
        }
        if (hasKeyConflict(full_key)) {
            m_last_error = "TOML key conflicts with existing key at line " + std::to_string(line_num) + ": " + full_key;
            return false;
        }
        m_values[full_key] = value;
        if (is_array) {
            m_arrays[full_key] = std::move(array_items);
        }
        return true;
    }

    std::string normalizeValue(const std::string& raw_value, int line_num, std::vector<std::string>* array_items) {
        if (raw_value.empty()) {
            m_last_error = "Empty TOML value at line " + std::to_string(line_num);
            return {};
        }

        if (raw_value.front() == '[') {
            if (raw_value.back() != ']') {
                m_last_error = "Invalid TOML array at line " + std::to_string(line_num);
                return {};
            }
            std::string array_body = raw_value.substr(1, raw_value.length() - 2);
            if (parser_detail::trim(array_body).empty()) {
                if (array_items != nullptr) {
                    array_items->clear();
                }
                return {};
            }
            if (hasNestedArray(array_body)) {
                m_last_error = "Unsupported nested TOML array at line " + std::to_string(line_num);
                return {};
            }
            auto items = splitTomlArrayItems(array_body);
            if (!items.empty() && items.back().empty() && parser_detail::trim(array_body).back() == ',') {
                items.pop_back();
            }
            ScalarKind array_kind = ScalarKind::Unknown;
            std::string result;
            for (size_t index = 0; index < items.size(); ++index) {
                ScalarKind item_kind = scalarKind(items[index]);
                if (item_kind == ScalarKind::Invalid) {
                    m_last_error = "Invalid TOML array item at line " + std::to_string(line_num);
                    return {};
                }
                if (array_kind == ScalarKind::Unknown) {
                    array_kind = item_kind;
                } else if (array_kind != item_kind) {
                    m_last_error = "Mixed TOML array item types at line " + std::to_string(line_num);
                    return {};
                }
                if (index > 0) {
                    result += ",";
                }
                std::string normalized_item = normalizeScalar(items[index], line_num);
                if (!m_last_error.empty()) {
                    return {};
                }
                result += normalized_item;
                if (array_items != nullptr) {
                    array_items->push_back(std::move(normalized_item));
                }
            }
            return result;
        }

        return normalizeScalar(raw_value, line_num);
    }

    std::string normalizeScalar(const std::string& raw_value, int line_num) {
        std::string value = parser_detail::trim(raw_value);
        if (value.empty()) {
            m_last_error = "Empty TOML scalar at line " + std::to_string(line_num);
            return {};
        }
        if (value.front() == '"' || value.front() == '\'') {
            if (!isTerminalQuotedTomlString(value)) {
                m_last_error = "Unterminated TOML string at line " + std::to_string(line_num);
                return {};
            }
            if (value.front() == '"' && !hasValidEscapes(value)) {
                m_last_error = "Invalid TOML escape at line " + std::to_string(line_num);
                return {};
            }
            if (!hasValidTomlStringQuotes(value)) {
                m_last_error = "Invalid TOML string at line " + std::to_string(line_num);
                return {};
            }
            return unquoteTomlString(value);
        }
        if (value.front() == '{' || value.back() == '}') {
            m_last_error = "Unsupported TOML inline table at line " + std::to_string(line_num);
            return {};
        }
        if (value.find('"') != std::string::npos || value.find('\'') != std::string::npos) {
            m_last_error = "Invalid TOML string at line " + std::to_string(line_num);
            return {};
        }
        if (value == "true" || value == "false" || isNumber(value)) {
            return value;
        }
        m_last_error = "Unsupported TOML scalar at line " + std::to_string(line_num) + ": " + value;
        return {};
    }

    enum class ScalarKind {
        Unknown,
        String,
        Bool,
        Number,
        Invalid
    };

    ScalarKind scalarKind(const std::string& raw_value) const {
        std::string value = parser_detail::trim(raw_value);
        if (value.empty() || value.front() == '[' || value.front() == '{' || value.back() == '}') {
            return ScalarKind::Invalid;
        }
        if (value.front() == '"' || value.front() == '\'') {
            if (!isTerminalQuotedTomlString(value)) {
                return ScalarKind::Invalid;
            }
            if (value.front() == '"' && !hasValidEscapes(value)) {
                return ScalarKind::Invalid;
            }
            if (!hasValidTomlStringQuotes(value)) {
                return ScalarKind::Invalid;
            }
            return ScalarKind::String;
        }
        if (value.find('"') != std::string::npos || value.find('\'') != std::string::npos) {
            return ScalarKind::Invalid;
        }
        if (value == "true" || value == "false") {
            return ScalarKind::Bool;
        }
        if (isNumber(value)) {
            return ScalarKind::Number;
        }
        return ScalarKind::Invalid;
    }

    static bool isValidKey(const std::string& key) {
        if (key.empty() || key.front() == '.' || key.back() == '.') {
            return false;
        }

        bool previous_dot = false;
        for (char character : key) {
            if (character == '.') {
                if (previous_dot) {
                    return false;
                }
                previous_dot = true;
                continue;
            }

            previous_dot = false;
            unsigned char byte = static_cast<unsigned char>(character);
            if (!std::isalnum(byte) && character != '_' && character != '-') {
                return false;
            }
        }
        return true;
    }

    static bool isNumber(const std::string& value) {
        size_t index = 0;
        if (value[index] == '+' || value[index] == '-') {
            ++index;
        }
        if (index >= value.length()) {
            return false;
        }
        if (index + 1 < value.length() && value[index] == '0' && std::isdigit(static_cast<unsigned char>(value[index + 1]))) {
            return false;
        }

        bool has_digit = false;
        while (index < value.length() && std::isdigit(static_cast<unsigned char>(value[index]))) {
            has_digit = true;
            ++index;
        }

        if (index < value.length() && value[index] == '.') {
            ++index;
            bool has_fraction_digit = false;
            while (index < value.length() && std::isdigit(static_cast<unsigned char>(value[index]))) {
                has_fraction_digit = true;
                ++index;
            }
            if (!has_fraction_digit) {
                return false;
            }
        }

        return has_digit && index == value.length();
    }

    static std::string stripTomlInlineComment(const std::string& text) {
        bool in_single_quote = false;
        bool in_double_quote = false;
        bool escaped = false;

        for (size_t index = 0; index < text.length(); ++index) {
            char character = text[index];

            if (in_double_quote && escaped) {
                escaped = false;
                continue;
            }
            if (in_double_quote && character == '\\') {
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
            if (character == '#' && !in_single_quote && !in_double_quote) {
                return parser_detail::trim(text.substr(0, index));
            }
        }

        return parser_detail::trim(text);
    }

    static std::vector<std::string> splitTomlArrayItems(const std::string& text) {
        if (parser_detail::trim(text).empty()) {
            return {};
        }

        std::vector<std::string> result;
        std::string item;
        bool in_single_quote = false;
        bool in_double_quote = false;
        bool escaped = false;

        for (char character : text) {
            if (in_double_quote && escaped) {
                item += character;
                escaped = false;
                continue;
            }
            if (in_double_quote && character == '\\') {
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
                result.push_back(parser_detail::trim(item));
                item.clear();
                continue;
            }

            item += character;
        }

        result.push_back(parser_detail::trim(item));
        return result;
    }

    static bool isTerminalQuotedTomlString(const std::string& value) {
        if (value.length() < 2 || (value.front() != '"' && value.front() != '\'') || value.back() != value.front()) {
            return false;
        }
        if (value.front() == '\'') {
            return true;
        }

        size_t slash_count = 0;
        for (size_t index = value.length() - 2; index > 0 && value[index] == '\\'; --index) {
            ++slash_count;
        }
        return slash_count % 2 == 0;
    }

    static bool hasValidTomlStringQuotes(const std::string& value) {
        char quote = value.front();
        if (quote == '\'') {
            for (size_t index = 1; index + 1 < value.length(); ++index) {
                if (value[index] == '\'') {
                    return false;
                }
            }
            return true;
        }

        bool escaped = false;
        for (size_t index = 1; index + 1 < value.length(); ++index) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (value[index] == '\\') {
                escaped = true;
                continue;
            }
            if (value[index] == '"') {
                return false;
            }
        }
        return !escaped;
    }

    static std::string unquoteTomlString(const std::string& value) {
        if (value.front() == '\'') {
            return value.substr(1, value.length() - 2);
        }
        return parser_detail::processEscapes(value.substr(1, value.length() - 2));
    }

    static bool hasValidEscapes(const std::string& value) {
        for (size_t index = 1; index + 1 < value.length(); ++index) {
            if (value[index] != '\\') {
                continue;
            }

            char escaped = value[++index];
            switch (escaped) {
                case 'b':
                case 't':
                case 'n':
                case 'f':
                case 'r':
                case '"':
                case '\\':
                    break;
                case 'u':
                    if (!hasHexDigits(value, index + 1, 4)) {
                        return false;
                    }
                    index += 4;
                    break;
                case 'U':
                    if (!hasHexDigits(value, index + 1, 8)) {
                        return false;
                    }
                    index += 8;
                    break;
                default:
                    return false;
            }
        }
        return true;
    }

    static bool hasHexDigits(const std::string& value, size_t start, size_t count) {
        if (start + count > value.length() - 1) {
            return false;
        }
        for (size_t index = start; index < start + count; ++index) {
            if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
                return false;
            }
        }
        return true;
    }

    static bool hasClosedArrayValue(const std::string& raw_value) {
        std::string value = parser_detail::trim(raw_value);
        if (value.empty() || value.front() != '[') {
            return true;
        }

        int depth = 0;
        bool in_single_quote = false;
        bool in_double_quote = false;
        bool escaped = false;

        for (char character : value) {
            if (in_double_quote && escaped) {
                escaped = false;
                continue;
            }
            if (in_double_quote && character == '\\') {
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
            if (in_single_quote || in_double_quote) {
                continue;
            }
            if (character == '[') {
                ++depth;
                continue;
            }
            if (character == ']') {
                --depth;
                if (depth <= 0) {
                    return true;
                }
            }
        }

        return false;
    }

    bool hasKeyConflict(const std::string& key) const {
        for (const auto& entry : m_values) {
            const std::string& existing_key = entry.first;
            if (existing_key == key) {
                return true;
            }
            if (existing_key.length() > key.length() &&
                existing_key.compare(0, key.length(), key) == 0 &&
                existing_key[key.length()] == '.') {
                return true;
            }
            if (key.length() > existing_key.length() &&
                key.compare(0, existing_key.length(), existing_key) == 0 &&
                key[existing_key.length()] == '.') {
                return true;
            }
        }
        return false;
    }

    static bool hasNestedArray(const std::string& value) {
        bool in_single_quote = false;
        bool in_double_quote = false;
        bool escaped = false;
        for (char character : value) {
            if (in_double_quote && escaped) {
                escaped = false;
                continue;
            }
            if (in_double_quote && character == '\\') {
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
            if ((character == '[' || character == ']') && !in_single_quote && !in_double_quote) {
                return true;
            }
        }
        if (in_single_quote || in_double_quote) {
            return true;
        }
        return false;
    }

    std::unordered_map<std::string, std::string> m_values;
    std::unordered_map<std::string, std::vector<std::string>> m_arrays;
    std::unordered_set<std::string> m_sections;
};

} // namespace galay::utils

#endif // GALAY_UTILS_PARSER_TOML_HPP
