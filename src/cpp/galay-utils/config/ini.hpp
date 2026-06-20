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
};

} // namespace galay::utils

#endif // GALAY_UTILS_PARSER_INI_HPP
