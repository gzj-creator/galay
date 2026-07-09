/**
 * @file ini.hpp
 * @brief INI 文件解析器
 * @author galay-utils
 * @version 1.0.0
 *
 * @details INI 文件解析器，继承自 ConfigParser，行为与 ConfigParser 一致。
 */

#ifndef GALAY_UTILS_PARSER_INI_HPP
#define GALAY_UTILS_PARSER_INI_HPP

#include "config.hpp"

namespace galay::utils {

/**
 * @brief INI 文件解析器
 * @details 继承自 ConfigParser，INI 文件行为与 ConfigParser 一致。
 */
class IniParser : public ConfigParser {
public:
    IniParser() = default;
    IniParser(IniParser&&) noexcept = default;
    IniParser& operator=(IniParser&&) noexcept = default;

    /**
     * @brief 显式克隆 INI 解析状态
     * @return 独立 IniParser 副本
     */
    [[nodiscard]] IniParser clone() const {
        IniParser copy;
        copy.m_values = m_values;
        copy.m_last_error = m_last_error;
        return copy;
    }

private:
    IniParser(const IniParser&) = delete;
    IniParser& operator=(const IniParser&) = delete;
};

} // namespace galay::utils

#endif // GALAY_UTILS_PARSER_INI_HPP
