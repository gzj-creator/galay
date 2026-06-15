/**
 * @file env.hpp
 * @brief 环境变量文件解析器
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 解析 .env 格式文件，支持 KEY=VALUE 和 export KEY=VALUE 行，
 *          # 注释和引号去除。
 */

#ifndef GALAY_UTILS_PARSER_ENV_HPP
#define GALAY_UTILS_PARSER_ENV_HPP

#include "galay-utils/config/detail.hpp"
#include "galay-utils/config/parser_base.hpp"
#include <unordered_map>

namespace galay::utils {

/**
 * @brief 环境变量文件解析器
 * @details 解析 .env 格式文件，支持 KEY=VALUE 和 export KEY=VALUE 行，
 *          # 注释和引号去除。
 */
class EnvParser : public ParserBase {
public:
    EnvParser() = default;

    bool parseFile(const std::string& path) override {
        return parseFileContent(path);
    }

    bool parseString(const std::string& content) override {
        m_values.clear();
        m_last_error.clear();

        std::istringstream input(content);
        std::string line;

        while (std::getline(input, line)) {
            line = parser_detail::trim(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }

            if (line.substr(0, 7) == "export ") {
                line = parser_detail::trim(line.substr(7));
            }

            auto equal_pos = line.find('=');
            if (equal_pos == std::string::npos) {
                continue;
            }

            std::string key = parser_detail::trim(line.substr(0, equal_pos));
            std::string value = parser_detail::trim(line.substr(equal_pos + 1));
            m_values[key] = parser_detail::unquote(value);
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

private:
    std::unordered_map<std::string, std::string> m_values;
};

} // namespace galay::utils

#endif // GALAY_UTILS_PARSER_ENV_HPP
