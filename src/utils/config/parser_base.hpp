/**
 * @file parser_base.hpp
 * @brief 解析器基类接口
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 定义键值对配置解析器的通用接口，包括文件/字符串解析、
 *          键值查询、类型转换和错误信息。
 */

#ifndef GALAY_UTILS_PARSER_BASE_HPP
#define GALAY_UTILS_PARSER_BASE_HPP

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace galay::utils {

/**
 * @brief 键值对配置解析器基类
 * @details 定义解析、查询和类型转换的通用接口。
 *          实例非线程安全，多线程环境下需外部同步或使用独立实例。
 */
class ParserBase {
public:
    virtual ~ParserBase() = default;

    /**
     * @brief 从文件解析配置内容
     * @param path 文件路径
     * @return 解析成功返回 true
     */
    virtual bool parseFile(const std::string& path) = 0;

    /**
     * @brief 从字符串解析配置内容
     * @param content 配置文本
     * @return 解析成功返回 true
     */
    virtual bool parseString(const std::string& content) = 0;

    /**
     * @brief 获取键对应的字符串值
     * @param key 键名（支持点分节表示法）
     * @return 值字符串，不存在时返回 std::nullopt
     */
    virtual std::optional<std::string> getValue(const std::string& key) const = 0;

    /**
     * @brief 检查键是否存在
     * @param key 键名
     * @return 存在返回 true
     */
    virtual bool hasKey(const std::string& key) const = 0;

    /**
     * @brief 获取所有键名
     * @return 键名列表（顺序未定义）
     */
    virtual std::vector<std::string> getKeys() const = 0;

    /**
     * @brief 获取键值并转换为指定类型
     * @tparam T 目标类型
     * @param key 键名
     * @param defaultValue 转换失败或键不存在时的默认值
     * @return 转换后的值或默认值
     */
    template<typename T>
    T getValueAs(const std::string& key, T defaultValue = T{}) const {
        auto value = getValue(key);
        if (!value) {
            return defaultValue;
        }

        std::istringstream input(*value);
        T result;
        if (input >> result) {
            return result;
        }
        return defaultValue;
    }

    /**
     * @brief 获取最后一次解析或文件错误
     * @return 错误信息字符串，无错误时为空
     */
    const std::string& lastError() const { return m_last_error; }

protected:
    bool parseFileContent(const std::string& path) {
        std::ifstream file(path);
        if (!file) {
            m_last_error = "Failed to open file: " + path;
            return false;
        }

        std::ostringstream output;
        output << file.rdbuf();
        return parseString(output.str());
    }

    std::string m_last_error;
};

} // namespace galay::utils

#endif // GALAY_UTILS_PARSER_BASE_HPP
