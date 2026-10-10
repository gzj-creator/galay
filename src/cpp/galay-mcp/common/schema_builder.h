/**
 * @file schema_builder.h
 * @brief JSON Schema与提示参数构建器
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 提供链式调用API用于构建JSON Schema和MCP提示参数定义，
 *          支持字符串、数字、整数、布尔、数组、对象、枚举等属性类型。
 */

#ifndef GALAY_MCP_COMMON_MCPSCHEMABUILDER_H
#define GALAY_MCP_COMMON_MCPSCHEMABUILDER_H

#include "mcp_base.h"
#include "mcp_json.h"
#include <string>
#include <vector>

namespace galay::mcp {

/**
 * @brief JSON Schema 构建器
 *
 * 提供链式调用方法来简化 JSON Schema 的构建
 */
class SchemaBuilder {
public:
    SchemaBuilder() = default;
    SchemaBuilder(SchemaBuilder&&) noexcept = default;
    SchemaBuilder& operator=(SchemaBuilder&&) noexcept = default;

    /**
     * @brief 显式复制当前 Schema 构建状态
     * @return 独立的 SchemaBuilder 副本
     */
    SchemaBuilder clone() const {
        SchemaBuilder copy;
        copy.m_properties = m_properties;
        return copy;
    }

    /**
     * @brief 添加字符串属性
     * @param name 属性名称
     * @param description 属性描述
     * @param required 是否为必填属性
     * @return 当前构建器引用，支持链式调用
     */
    SchemaBuilder& add_string(const std::string& name,
                             const std::string& description,
                             bool required = false) {
        Property prop;
        prop.kind = PropertyKind::String;
        prop.name = name;
        prop.description = description;
        prop.required = required;
        m_properties.push_back(std::move(prop));
        return *this;
    }

    /**
     * @brief 添加数字属性
     * @param name 属性名称
     * @param description 属性描述
     * @param required 是否为必填属性
     * @return 当前构建器引用，支持链式调用
     */
    SchemaBuilder& add_number(const std::string& name,
                             const std::string& description,
                             bool required = false) {
        Property prop;
        prop.kind = PropertyKind::Number;
        prop.name = name;
        prop.description = description;
        prop.required = required;
        m_properties.push_back(std::move(prop));
        return *this;
    }

    /**
     * @brief 添加整数属性
     * @param name 属性名称
     * @param description 属性描述
     * @param required 是否为必填属性
     * @return 当前构建器引用，支持链式调用
     */
    SchemaBuilder& add_integer(const std::string& name,
                              const std::string& description,
                              bool required = false) {
        Property prop;
        prop.kind = PropertyKind::Integer;
        prop.name = name;
        prop.description = description;
        prop.required = required;
        m_properties.push_back(std::move(prop));
        return *this;
    }

    /**
     * @brief 添加布尔属性
     * @param name 属性名称
     * @param description 属性描述
     * @param required 是否为必填属性
     * @return 当前构建器引用，支持链式调用
     */
    SchemaBuilder& add_boolean(const std::string& name,
                              const std::string& description,
                              bool required = false) {
        Property prop;
        prop.kind = PropertyKind::Boolean;
        prop.name = name;
        prop.description = description;
        prop.required = required;
        m_properties.push_back(std::move(prop));
        return *this;
    }

    /**
     * @brief 添加数组属性
     * @param name 属性名称
     * @param description 属性描述
     * @param itemType 数组元素类型，默认为"string"
     * @param required 是否为必填属性
     * @return 当前构建器引用，支持链式调用
     */
    SchemaBuilder& add_array(const std::string& name,
                            const std::string& description,
                            const std::string& itemType = "string",
                            bool required = false) {
        Property prop;
        prop.kind = PropertyKind::Array;
        prop.name = name;
        prop.description = description;
        prop.itemType = itemType;
        prop.required = required;
        m_properties.push_back(std::move(prop));
        return *this;
    }

    /**
     * @brief 添加对象属性（使用已有Schema JSON）
     * @param name 属性名称
     * @param description 属性描述
     * @param objectSchema 对象的JSON Schema字符串
     * @param required 是否为必填属性
     * @return 当前构建器引用，支持链式调用
     */
    SchemaBuilder& add_object(const std::string& name,
                             const std::string& description,
                             const std::string& objectSchema,
                             bool required = false) {
        Property prop;
        prop.kind = PropertyKind::Object;
        prop.name = name;
        prop.description = description;
        prop.objectSchema = objectSchema;
        prop.required = required;
        m_properties.push_back(std::move(prop));
        return *this;
    }

    /**
     * @brief 添加对象属性（使用SchemaBuilder）
     * @param name 属性名称
     * @param description 属性描述
     * @param objectSchema 子构建器，自动调用build()
     * @param required 是否为必填属性
     * @return 当前构建器引用，支持链式调用
     */
    SchemaBuilder& add_object(const std::string& name,
                             const std::string& description,
                             const SchemaBuilder& objectSchema,
                             bool required = false) {
        return add_object(name, description, objectSchema.build(), required);
    }

    /**
     * @brief 添加枚举属性
     * @param name 属性名称
     * @param description 属性描述
     * @param enumValues 枚举值列表
     * @param required 是否为必填属性
     * @return 当前构建器引用，支持链式调用
     */
    SchemaBuilder& add_enum(const std::string& name,
                           const std::string& description,
                           const std::vector<std::string>& enumValues,
                           bool required = false) {
        Property prop;
        prop.kind = PropertyKind::Enum;
        prop.name = name;
        prop.description = description;
        prop.enumValues = enumValues;
        prop.required = required;
        m_properties.push_back(std::move(prop));
        return *this;
    }

    /**
     * @brief 构建最终的 Schema
     * @return JSON Schema 字符串
     */
    std::string build() const {
        std::string out;
        auto writer = make_json_writer(out);
        // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
        (void)writer.start_object();
        (void)writer.key("type");
        (void)writer.string("object");
        (void)writer.key("properties");
        (void)writer.start_object();
        for (const auto& prop : m_properties) {
            (void)writer.key(prop.name);
            write_property(writer, prop);
        }
        (void)writer.end_object();

        bool hasRequired = false;
        for (const auto& prop : m_properties) {
            if (prop.required) {
                hasRequired = true;
                break;
            }
        }
        if (hasRequired) {
            (void)writer.key("required");
            (void)writer.start_array();
            for (const auto& prop : m_properties) {
                if (prop.required) {
                    (void)writer.string(prop.name);
                }
            }
            (void)writer.end_array();
        }
        (void)writer.end_object();
        if (!writer.finish()) {
            return std::string{};
        }
        return std::move(out);
    }

private:
    SchemaBuilder(const SchemaBuilder&) = delete;
    SchemaBuilder& operator=(const SchemaBuilder&) = delete;

    /**
     * @brief 属性类型枚举
     */
    enum class PropertyKind {
        String, ///< 字符串类型
        Number, ///< 数字类型
        Integer, ///< 整数类型
        Boolean, ///< 布尔类型
        Array, ///< 数组类型
        Object, ///< 对象类型
        Enum ///< 枚举类型
    };

    /**
     * @brief 属性定义结构
     */
    struct Property {
        PropertyKind kind{PropertyKind::String}; ///< 属性类型
        std::string name; ///< 属性名称
        std::string description; ///< 属性描述
        bool required{false}; ///< 是否必填
        std::string itemType; ///< 数组元素类型（Array类型使用）
        std::vector<std::string> enumValues; ///< 枚举值列表（Enum类型使用）
        std::string objectSchema; ///< 对象Schema（Object类型使用）
    };

    /**
     * @brief 将单个属性写入JSON
     * @param writer JSON写入器
     * @param prop 属性定义
     * @return 无返回值
     */
    static void write_property(json::stream::StreamWriter& writer, const Property& prop) {
        // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
        if (prop.kind == PropertyKind::Object && !prop.objectSchema.empty()) {
            if (prop.description.empty()) {
                (void)writer.raw(prop.objectSchema);
                return;
            }

            auto parsed = JsonDocument::parse(prop.objectSchema);
            if (!parsed) {
                (void)writer.raw(prop.objectSchema);
                return;
            }

            if (!parsed.value().root().is_object()) {
                (void)writer.raw(prop.objectSchema);
                return;
            }
            const json::Json& obj = parsed.value().root();

            std::string mergedOut;
            auto merged = make_json_writer(mergedOut);
            // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
            (void)merged.start_object();
            (void)merged.key("description");
            (void)merged.string(prop.description);
            obj.for_each_member([&](std::string_view key, const json::Json& value) -> json::result<void> {
                std::string raw;
                auto serialized = json::stream::serialize(value, [&](std::string_view chunk) -> json::result<void> {
                    raw.append(chunk);
                    return {};
                });
                if (serialized) {
                    (void)merged.key(key);
                    (void)merged.raw(raw);
                }
                return {};
            });
            (void)merged.end_object();
            if (!merged.finish()) {
                (void)writer.raw(prop.objectSchema);
                return;
            }
            (void)writer.raw(mergedOut);
            return;
        }

        (void)writer.start_object();
        (void)writer.key("type");
        switch (prop.kind) {
            case PropertyKind::String:
                (void)writer.string("string");
                break;
            case PropertyKind::Number:
                (void)writer.string("number");
                break;
            case PropertyKind::Integer:
                (void)writer.string("integer");
                break;
            case PropertyKind::Boolean:
                (void)writer.string("boolean");
                break;
            case PropertyKind::Array:
                (void)writer.string("array");
                break;
            case PropertyKind::Enum:
                (void)writer.string("string");
                break;
            case PropertyKind::Object:
                (void)writer.string("object");
                break;
        }

        if (!prop.description.empty()) {
            (void)writer.key("description");
            (void)writer.string(prop.description);
        }

        if (prop.kind == PropertyKind::Array) {
            (void)writer.key("items");
            (void)writer.start_object();
            (void)writer.key("type");
            (void)writer.string(prop.itemType.empty() ? "string" : prop.itemType);
            (void)writer.end_object();
        }

        if (prop.kind == PropertyKind::Enum) {
            (void)writer.key("enum");
            (void)writer.start_array();
            for (const auto& value : prop.enumValues) {
                (void)writer.string(value);
            }
            (void)writer.end_array();
        }

        (void)writer.end_object();
    }

    std::vector<Property> m_properties; ///< 属性列表
};

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
