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

namespace galay {
namespace mcp {

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
    SchemaBuilder& addString(const std::string& name,
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
    SchemaBuilder& addNumber(const std::string& name,
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
    SchemaBuilder& addInteger(const std::string& name,
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
    SchemaBuilder& addBoolean(const std::string& name,
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
    SchemaBuilder& addArray(const std::string& name,
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
    SchemaBuilder& addObject(const std::string& name,
                             const std::string& description,
                             const JsonString& objectSchema,
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
    SchemaBuilder& addObject(const std::string& name,
                             const std::string& description,
                             const SchemaBuilder& objectSchema,
                             bool required = false) {
        return addObject(name, description, objectSchema.build(), required);
    }

    /**
     * @brief 添加枚举属性
     * @param name 属性名称
     * @param description 属性描述
     * @param enumValues 枚举值列表
     * @param required 是否为必填属性
     * @return 当前构建器引用，支持链式调用
     */
    SchemaBuilder& addEnum(const std::string& name,
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
    JsonString build() const {
        JsonWriter writer;
        writer.startObject();
        writer.key("type");
        writer.string("object");
        writer.key("properties");
        writer.startObject();
        for (const auto& prop : m_properties) {
            writer.key(prop.name);
            writeProperty(writer, prop);
        }
        writer.endObject();

        bool hasRequired = false;
        for (const auto& prop : m_properties) {
            if (prop.required) {
                hasRequired = true;
                break;
            }
        }
        if (hasRequired) {
            writer.key("required");
            writer.startArray();
            for (const auto& prop : m_properties) {
                if (prop.required) {
                    writer.string(prop.name);
                }
            }
            writer.endArray();
        }
        writer.endObject();
        return writer.takeString();
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
        JsonString objectSchema; ///< 对象Schema（Object类型使用）
    };

    /**
     * @brief 将单个属性写入JSON
     * @param writer JSON写入器
     * @param prop 属性定义
     */
    static void writeProperty(JsonWriter& writer, const Property& prop) {
        if (prop.kind == PropertyKind::Object && !prop.objectSchema.empty()) {
            if (prop.description.empty()) {
                writer.raw(prop.objectSchema);
                return;
            }

            auto parsed = JsonDocument::parse(prop.objectSchema);
            if (!parsed) {
                writer.raw(prop.objectSchema);
                return;
            }

            JsonObject obj;
            if (!JsonHelper::getObject(parsed.value().root(), obj)) {
                writer.raw(prop.objectSchema);
                return;
            }

            JsonWriter merged;
            merged.startObject();
            merged.key("description");
            merged.string(prop.description);
            for (auto field : obj) {
                std::string raw;
                if (JsonHelper::getRawJson(field.value, raw)) {
                    merged.key(std::string(field.key));
                    merged.raw(raw);
                }
            }
            merged.endObject();
            writer.raw(merged.takeString());
            return;
        }

        writer.startObject();
        writer.key("type");
        switch (prop.kind) {
            case PropertyKind::String:
                writer.string("string");
                break;
            case PropertyKind::Number:
                writer.string("number");
                break;
            case PropertyKind::Integer:
                writer.string("integer");
                break;
            case PropertyKind::Boolean:
                writer.string("boolean");
                break;
            case PropertyKind::Array:
                writer.string("array");
                break;
            case PropertyKind::Enum:
                writer.string("string");
                break;
            case PropertyKind::Object:
                writer.string("object");
                break;
        }

        if (!prop.description.empty()) {
            writer.key("description");
            writer.string(prop.description);
        }

        if (prop.kind == PropertyKind::Array) {
            writer.key("items");
            writer.startObject();
            writer.key("type");
            writer.string(prop.itemType.empty() ? "string" : prop.itemType);
            writer.endObject();
        }

        if (prop.kind == PropertyKind::Enum) {
            writer.key("enum");
            writer.startArray();
            for (const auto& value : prop.enumValues) {
                writer.string(value);
            }
            writer.endArray();
        }

        writer.endObject();
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
    PromptArgumentBuilder& addArgument(const std::string& name,
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

} // namespace mcp
} // namespace galay
#endif // GALAY_MCP_COMMON_MCPSCHEMABUILDER_H
