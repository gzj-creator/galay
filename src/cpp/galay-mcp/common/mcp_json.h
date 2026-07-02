/**
 * @file mcp_json.h
 * @brief JSON文档解析、写入与辅助工具
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 基于simdjson库提供高性能的JSON解析能力，
 *          同时提供手写的JsonWriter用于序列化和JsonHelper用于字段访问。
 */

#ifndef GALAY_MCP_COMMON_MCPJSON_H
#define GALAY_MCP_COMMON_MCPJSON_H

#include "mcp_error.h"
#include <simdjson.h>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace galay {
namespace mcp {

using JsonString = std::string; ///< JSON字符串类型别名
using JsonElement = simdjson::dom::element; ///< JSON元素类型别名
using JsonObject = simdjson::dom::object; ///< JSON对象类型别名
using JsonArray = simdjson::dom::array; ///< JSON数组类型别名

/**
 * @brief JSON文档类
 * @details 持有simdjson文档和缓冲区，提供JSON文本的解析和访问功能
 */
class JsonDocument {
public:
    JsonDocument() = default; ///< 默认构造
    JsonDocument(JsonDocument&& other) noexcept; ///< 移动构造，转移地址稳定的DOM存储
    JsonDocument& operator=(JsonDocument&& other) noexcept; ///< 移动赋值，转移地址稳定的DOM存储
    JsonDocument(const JsonDocument&) = delete; ///< 禁止拷贝底层DOM存储
    JsonDocument& operator=(const JsonDocument&) = delete; ///< 禁止拷贝底层DOM存储

    /**
     * @brief 解析JSON文本创建文档
     * @param json 原始JSON字符串视图
     * @return 成功返回JsonDocument，失败返回McpError
     */
    static std::expected<JsonDocument, McpError> parse(std::string_view json);

    const JsonElement& root() const { return m_root; } ///< 获取根元素（只读）
    JsonElement& root() { return m_root; } ///< 获取根元素（可修改）
    std::string_view raw() const { return std::string_view(m_buffer.data(), m_buffer.size()); } ///< 获取原始JSON文本

private:
    simdjson::padded_string m_buffer; ///< 带填充的缓冲区
    std::unique_ptr<simdjson::dom::document> m_document; ///< 地址稳定的DOM存储，移动JsonDocument不失效外部元素视图
    JsonElement m_root; ///< 根元素
};

/**
 * @brief JSON写入器
 * @details 提供流式API用于手动构建JSON字符串，支持对象、数组、各种值类型
 */
class JsonWriter {
public:
    void startObject(); ///< 开始写入JSON对象
    void endObject(); ///< 结束JSON对象
    void startArray(); ///< 开始写入JSON数组
    void endArray(); ///< 结束JSON数组
    void key(const std::string& key); ///< 写入对象键名
    void string(const std::string& value); ///< 写入字符串值
    void number(int64_t value); ///< 写入有符号整数值
    void number(uint64_t value); ///< 写入无符号整数值
    void number(double value); ///< 写入浮点数值
    void boolean(bool value); ///< 写入布尔值
    void nullValue(); ///< 写入null值
    void raw(const std::string& json); ///< 写入原始JSON片段

    /**
     * @brief 取出构建的JSON字符串并清空写入器状态
     * @return 完整的JSON字符串
     */
    std::string takeString();

private:
    /**
     * @brief 上下文类型枚举
     */
    enum class ContextType {
        Object, ///< 对象上下文
        Array ///< 数组上下文
    };

    /**
     * @brief 写入上下文结构
     */
    struct Context {
        ContextType type; ///< 上下文类型
        bool first = true; ///< 是否为第一个元素
        bool expectValue = false; ///< 是否期望写入值（对象Key之后）
    };

    void writeValuePrefix(); ///< 写入值前缀
    void writeCommaIfNeeded(); ///< 按需写入逗号分隔符

    /**
     * @brief 将字符串转义后追加到输出
     * @param out 目标输出字符串
     * @param value 原始字符串值
     */
    static void appendEscaped(std::string& out, const std::string& value);

    std::string m_out; ///< 输出缓冲区
    std::vector<Context> m_stack; ///< 嵌套上下文栈
};

/**
 * @brief JSON辅助工具类
 * @details 提供从JsonElement和JsonObject中安全提取各种类型值的静态方法
 */
class JsonHelper {
public:
    static bool getObject(const JsonElement& element, JsonObject& out); ///< 从元素获取JSON对象
    static bool getArray(const JsonElement& element, JsonArray& out); ///< 从元素获取JSON数组
    static bool getStringValue(const JsonElement& element, std::string& out); ///< 从元素获取字符串值
    static bool getRawJson(const JsonElement& element, std::string& out); ///< 从元素获取原始JSON文本

    static bool getString(const JsonObject& obj, const char* key, std::string& out); ///< 从对象按键获取字符串
    static bool getInt64(const JsonObject& obj, const char* key, int64_t& out); ///< 从对象按键获取整数
    static bool getBool(const JsonObject& obj, const char* key, bool& out); ///< 从对象按键获取布尔值
    static bool getElement(const JsonObject& obj, const char* key, JsonElement& out); ///< 从对象按键获取元素
    static bool getObject(const JsonObject& obj, const char* key, JsonObject& out); ///< 从对象按键获取嵌套对象
    static bool getArray(const JsonObject& obj, const char* key, JsonArray& out); ///< 从对象按键获取数组

    static const JsonElement& emptyObject(); ///< 获取空对象的静态引用
};

} // namespace mcp
} // namespace galay

#endif // GALAY_MCP_COMMON_MCPJSON_H
