/**
 * @file schema_builder.h
 * @brief MCP提示参数构建器
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 提供链式调用API用于构建MCP提示参数定义。
 */

#ifndef GALAY_MCP_COMMON_MCPSCHEMABUILDER_H
#define GALAY_MCP_COMMON_MCPSCHEMABUILDER_H

#include "mcp_base.h"
#include <string>
#include <vector>

namespace galay::mcp {

/**
 * @brief 提示参数构建器
 *
 * 用于构建 MCP 提示的参数定义
 */
class PromptArgumentBuilder {
public:
    PromptArgumentBuilder() = default;
    PromptArgumentBuilder(PromptArgumentBuilder&&) noexcept = default;
    PromptArgumentBuilder& operator=(PromptArgumentBuilder&&) noexcept = default;

    /**
     * @brief 显式复制当前提示参数构建状态
     * @return 独立的 PromptArgumentBuilder 副本
     */
    PromptArgumentBuilder clone() const {
        PromptArgumentBuilder copy;
        copy.m_arguments = m_arguments;
        return copy;
    }

    /**
     * @brief 添加提示参数
     * @param name 参数名称
     * @param description 参数描述
     * @param required 是否为必填参数
     * @return 当前构建器引用，支持链式调用
     */
    PromptArgumentBuilder& add_argument(const std::string& name,
                                       const std::string& description,
                                       bool required = false) {
        PromptArgument arg;
        arg.name = name;
        arg.description = description;
        arg.required = required;
        m_arguments.push_back(std::move(arg));
        return *this;
    }

    /**
     * @brief 构建最终的参数列表
     * @return 提示参数向量
     */
    std::vector<PromptArgument> build() const {
        return m_arguments;
    }

private:
    PromptArgumentBuilder(const PromptArgumentBuilder&) = delete;
    PromptArgumentBuilder& operator=(const PromptArgumentBuilder&) = delete;

    std::vector<PromptArgument> m_arguments; ///< 参数列表
};

} // namespace galay::mcp
#endif // GALAY_MCP_COMMON_MCPSCHEMABUILDER_H
