/**
 * @file parser_manager.hpp
 * @brief 解析器工厂和管理器
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 根据文件扩展名自动创建对应的解析器实例，
 *          默认注册 .conf、.ini、.env、.toml 四种解析器。
 */

#ifndef GALAY_UTILS_PARSER_MANAGER_HPP
#define GALAY_UTILS_PARSER_MANAGER_HPP

#include "config.hpp"
#include "env.hpp"
#include "ini.hpp"
#include "toml.hpp"
#include <functional>
#include <memory>
#include <unordered_map>

namespace galay::utils {

/**
 * @brief 解析器注册表和工厂
 * @details 单例模式，根据文件扩展名自动创建对应的解析器。
 *          默认注册 .conf、.ini、.env、.toml 四种解析器。
 */
class ParserManager {
public:
    using Creator = std::function<std::unique_ptr<ParserBase>()>; ///< 解析器工厂函数类型

    /**
     * @brief 获取单例实例
     * @return 解析器管理器实例引用
     */
    static ParserManager& instance() {
        static ParserManager instance;
        return instance;
    }

    /**
     * @brief 注册或替换指定扩展名的解析器工厂
     * @param extension 文件扩展名（含前导点，如 `.toml`）
     * @param creator 解析器工厂函数
     */
    void registerParser(const std::string& extension, Creator creator) {
        m_creators[extension] = std::move(creator);
    }

    /**
     * @brief 根据文件路径的扩展名创建解析器
     * @param path 文件路径或文件名
     * @return 解析器实例，未知扩展名返回 nullptr
     */
    std::unique_ptr<ParserBase> createParser(const std::string& path) {
        auto dot_pos = path.rfind('.');
        if (dot_pos == std::string::npos) {
            return nullptr;
        }

        std::string extension = path.substr(dot_pos);
        auto iter = m_creators.find(extension);
        if (iter != m_creators.end()) {
            return iter->second();
        }

        return nullptr;
    }

private:
    ParserManager() {
        registerParser(".conf", []() { return std::make_unique<ConfigParser>(); });
        registerParser(".ini", []() { return std::make_unique<IniParser>(); });
        registerParser(".env", []() { return std::make_unique<EnvParser>(); });
        registerParser(".toml", []() { return std::make_unique<TomlParser>(); });
    }

    std::unordered_map<std::string, Creator> m_creators;
};

} // namespace galay::utils

#endif // GALAY_UTILS_PARSER_MANAGER_HPP
