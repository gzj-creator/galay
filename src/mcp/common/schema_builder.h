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

#include "mcp/common/mcp_base.h"
#include "mcp/common/mcp_json.h"
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
        writer.StartObject();
        writer.Key("type");
        writer.String("object");
        writer.Key("properties");
        writer.StartObject();
        for (const auto& prop : m_properties) {
            writer.Key(prop.name);
            writeProperty(writer, prop);
        }
        writer.EndObject();

        bool hasRequired = false;
        for (const auto& prop : m_properties) {
            if (prop.required) {
                hasRequired = true;
                break;
            }
        }
        if (hasRequired) {
            writer.Key("required");
            writer.StartArray();
            for (const auto& prop : m_properties) {
                if (prop.required) {
                    writer.String(prop.name);
                }
            }
            writer.EndArray();
        }
        writer.EndObject();
        return writer.TakeString();
    }

private:
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
                writer.Raw(prop.objectSchema);
                return;
            }

            auto parsed = JsonDocument::Parse(prop.objectSchema);
            if (!parsed) {
                writer.Raw(prop.objectSchema);
                return;
            }

            JsonObject obj;
            if (!JsonHelper::GetObject(parsed.value().Root(), obj)) {
                writer.Raw(prop.objectSchema);
                return;
            }

            JsonWriter merged;
            merged.StartObject();
            merged.Key("description");
            merged.String(prop.description);
            for (auto field : obj) {
                std::string raw;
                if (JsonHelper::GetRawJson(field.value, raw)) {
                    merged.Key(std::string(field.key));
                    merged.Raw(raw);
                }
            }
            merged.EndObject();
            writer.Raw(merged.TakeString());
            return;
        }

        writer.StartObject();
        writer.Key("type");
        switch (prop.kind) {
            case PropertyKind::String:
                writer.String("string");
                break;
            case PropertyKind::Number:
                writer.String("number");
                break;
            case PropertyKind::Integer:
                writer.String("integer");
                break;
            case PropertyKind::Boolean:
                writer.String("boolean");
                break;
            case PropertyKind::Array:
                writer.String("array");
                break;
            case PropertyKind::Enum:
                writer.String("string");
                break;
            case PropertyKind::Object:
                writer.String("object");
                break;
        }

        if (!prop.description.empty()) {
            writer.Key("description");
            writer.String(prop.description);
        }

        if (prop.kind == PropertyKind::Array) {
            writer.Key("items");
            writer.StartObject();
            writer.Key("type");
            writer.String(prop.itemType.empty() ? "string" : prop.itemType);
            writer.EndObject();
        }

        if (prop.kind == PropertyKind::Enum) {
            writer.Key("enum");
            writer.StartArray();
            for (const auto& value : prop.enumValues) {
                writer.String(value);
            }
            writer.EndArray();
        }

        writer.EndObject();
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
    std::vector<PromptArgument> m_arguments; ///< 参数列表
};

} // namespace mcp
} // namespace galay
#endif // GALAY_MCP_COMMON_MCPSCHEMABUILDER_H
