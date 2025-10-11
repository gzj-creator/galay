#ifndef __GALAY_PARSER_H__
#define __GALAY_PARSER_H__

#include <any>
#include <fstream>
#include <filesystem>
#include <string.h>
#include <sstream>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <functional>

#ifdef INCLUDE_NLOHMANN_JSON_HPP
    #define USE_NLOHMANN_JSON
#endif

#ifdef USE_NLOHMANN_JSON
    #include <nlohmann/json.hpp>
#endif

namespace galay::parser
{
    /**
     * @brief 解析器基类
     * @details 定义配置文件解析器的通用接口
     */
    class ParserBase
    {
    public:
        using ptr = std::shared_ptr<ParserBase>;
        using wptr = std::weak_ptr<ParserBase>;
        using uptr = std::unique_ptr<ParserBase>;
        
        /**
         * @brief 解析配置文件
         * @param filename 文件名或文件路径
         * @return 解析结果，0表示成功
         */
        virtual int parse(const std::string &filename) = 0;
        
        /**
         * @brief 获取配置项的值
         * @param key 配置项键名
         * @return std::any类型的值
         */
        virtual std::any getValue(const std::string& key) = 0;
    };

    template <typename T>
    concept ParserFromBase = std::is_base_of_v<ParserBase, T>;

    /**
     * @brief 解析器管理器
     * @details 管理不同类型配置文件的解析器，支持动态注册和创建解析器
     */
    class ParserManager{
    public:
        using ptr = std::shared_ptr<ParserManager>;
        using uptr = std::unique_ptr<ParserManager>;
        using wptr = std::weak_ptr<ParserManager>;
        
        ParserManager();

        /**
         * @brief 创建解析器并可选地执行解析
         * @param filename 文件名（包含扩展名，如".json"）
         * @param IsParse true: 创建后立即调用parse()，false: 仅创建不解析
         * @return 成功返回解析器指针，失败返回nullptr
         */
        ParserBase::ptr createParser(const std::string &filename, bool IsParse = true);

        /**
         * @brief 注册文件扩展名对应的解析器类型
         * @tparam T 解析器类型（必须继承自ParserBase）
         * @param ext 文件扩展名（如".json"、".conf"）
         */
        template<ParserFromBase T>
        void registerExtension(const std::string& ext)
        {
            m_creater[ext] = []()->ParserBase::ptr{
                return std::make_shared<T>();
            };
        }
    private:
        std::unordered_map<std::string,std::function<ParserBase::ptr()>> m_creater;
    };

    /**
     * @brief 配置文件解析器
     * @details 解析键值对格式的配置文件，支持转义字符和数组
     */
    class ConfigParser : public ParserBase
    {
        enum ConfType
        {
            kConfKey,
            kConfValue,
        };

    public:
        /**
         * @brief 解析配置文件
         * @param filename 配置文件路径
         * @return 解析结果，0表示成功
         */
        int parse(const std::string &filename) override;
        
        /**
         * @brief 获取配置项的值
         * @param key 配置项键名
         * @return std::any类型的值
         */
        std::any getValue(const std::string &key) override;
        
        /**
         * @brief 获取配置项的值并转换为指定类型
         * @tparam T 目标类型
         * @param key 配置项键名
         * @return 转换后的值，转换失败返回默认构造的T类型对象
         */
        template<typename T>
        T getValueAs(const std::string& key) {
            try {
                return std::any_cast<T>(getValue(key));
            } catch (...) {
                return T();
            }
        }
    private:
        int parseContent(const std::string &content);
        std::string parseEscapes(const std::string& input);
        std::vector<std::string> parseArray(const std::string& arr_str);
    private:
        std::unordered_map<std::string, std::any> m_fields;
    };

#ifdef USE_NLOHMANN_JSON

    using JsonValue = nlohmann::json;
    class JsonParser : public ParserBase
    {
    public:
        int parse(const std::string &filename) override;
        std::any getValue(const std::string &key) override;
    private:
        nlohmann::json m_json;
    };


#endif

}

#endif