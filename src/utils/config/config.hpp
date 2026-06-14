/**
 * @file config.hpp
 * @brief 配置文件解析器
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 解析简单 .conf 格式的键值对配置文件，支持 [section] 分节、
 *          # 和 ; 注释行，支持逗号分隔的数组值。
 */

#ifndef GALAY_UTILS_PARSER_CONFIG_HPP
#define GALAY_UTILS_PARSER_CONFIG_HPP

#include "utils/config/detail.hpp"
#include "utils/config/parser_base.hpp"
#include <unordered_map>

namespace galay::utils {

/**
 * @brief 配置文件解析器
 * @details 解析简单 .conf 格式的键值对文件。支持 [section] 分节，
 *          # 和 ; 注释行，匹配的单双引号去除。
 */
class ConfigParser : public ParserBase {
public:
    ConfigParser() = default;

    bool parseFile(const std::string& path) override {
        return parseFileContent(path);
    }

    bool parseString(const std::string& content) override {
        m_values.clear();
        m_last_error.clear();
        std::string current_section;

        std::istringstream input(content);
        std::string line;
        int line_num = 0;

        while (std::getline(input, line)) {
            ++line_num;
            line = parser_detail::trim(line);

            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }

            if (line[0] == '[' && line.back() == ']') {
                current_section = parser_detail::trim(line.substr(1, line.length() - 2));
                continue;
            }

            auto equal_pos = line.find('=');
            if (equal_pos == std::string::npos) {
                m_last_error = "Invalid line " + std::to_string(line_num) + ": " + line;
                return false;
            }

            std::string key = parser_detail::trim(line.substr(0, equal_pos));
            std::string value = parser_detail::trim(line.substr(equal_pos + 1));
            value = parser_detail::unquote(value);

            std::string full_key = current_section.empty() ? key : current_section + "." + key;
            m_values[full_key] = value;
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

    /**
     * @brief 获取指定分节下的所有键名
     * @param section 分节名称
     * @return 键名列表
     */
    std::vector<std::string> getKeysInSection(const std::string& section) const {
        std::vector<std::string> keys;
        std::string prefix = section + ".";
        for (const auto& entry : m_values) {
            if (entry.first.substr(0, prefix.length()) == prefix) {
                keys.push_back(entry.first.substr(prefix.length()));
            }
        }
        return keys;
    }

    /**
     * @brief 获取逗号分隔的数组值
     * @param key 键名
     * @return 字符串数组
     */
    std::vector<std::string> getArray(const std::string& key) const {
        auto value = getValue(key);
        if (!value) {
            return {};
        }
        return parser_detail::splitCommaSeparated(*value);
    }

protected:
    std::unordered_map<std::string, std::string> m_values;
};

} // namespace galay::utils

#endif // GALAY_UTILS_PARSER_CONFIG_HPP
