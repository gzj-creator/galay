/**
 * @file mcp_json.h
 * @brief JSON文档解析与serde接入工具
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details JSON解析与序列化全部基于serde模块（galay-serde）：
 *          - 解析使用 json::parse / json::Json DOM；
 *          - 序列化使用 json::stream::StreamWriter 流式输出。
 *          本头只提供输出 sink 与常量的最小胶水，不含任何手写JSON语法逻辑。
 */

#ifndef GALAY_MCP_COMMON_MCPJSON_H
#define GALAY_MCP_COMMON_MCPJSON_H

#include "mcp_error.h"
#include <serde/json/json.hpp>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace galay::mcp {

/**
 * @brief JSON文档类
 * @details 持有serde解析的JSON值，根值拥有底层文档存储，
 *          移动JsonDocument不失效其派生的子值视图。
 */
class JsonDocument {
public:
    JsonDocument() = default; ///< 默认构造
    JsonDocument(JsonDocument&& other) noexcept = default; ///< 移动构造，转移地址稳定的DOM存储
    JsonDocument& operator=(JsonDocument&& other) noexcept = default; ///< 移动赋值，转移地址稳定的DOM存储
    JsonDocument(const JsonDocument&) = delete; ///< 禁止拷贝底层DOM存储
    JsonDocument& operator=(const JsonDocument&) = delete; ///< 禁止拷贝底层DOM存储

    /**
     * @brief 解析JSON文本创建文档
     * @param json 原始JSON字符串视图
     * @return 成功返回JsonDocument，失败返回McpError
     */
    static std::expected<JsonDocument, McpError> parse(std::string_view json);

    const json::Json& root() const noexcept { return m_root; } ///< 获取根元素（只读）
    json::Json& root() noexcept { return m_root; } ///< 获取根元素（可修改）

private:
    json::Json m_root; ///< 根元素，持有底层文档存储
};

/**
 * @brief 构造以字符串为输出sink的serde流式JSON写入器
 * @param out 接收JSON文本的字符串
 * @return 绑定sink的 json::stream::StreamWriter
 * @note StreamWriter失败粘滞：中间写入调用的返回值可安全丢弃，
 *       最终必须检查 finish() 的结果。
 */
inline json::stream::StreamWriter makeJsonWriter(std::string& out) {
    return json::stream::StreamWriter{[&out](std::string_view text) -> json::result<void> {
        out.append(text);
        return {};
    }};
}

/**
 * @brief 获取空JSON对象 "{}" 的静态引用
 * @return 常驻的空对象 serde JSON 值
 */
inline const json::Json& emptyJsonObject() {
    static const json::Json empty = []() {
        auto parsed = json::parse("{}");
        return parsed ? std::move(parsed.value()) : json::Json{};
    }();
    return empty;
}

} // namespace galay::mcp

#endif // GALAY_MCP_COMMON_MCPJSON_H
