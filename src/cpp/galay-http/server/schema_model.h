/**
 * @file schema_model.h
 * @brief OpenAPI Schema 的轻量数据模型。
 * @details 该模型位于 HTTP 契约层，不依赖 serde；可选文档模块负责从 DTO 推导和序列化它。
 */

#ifndef GALAY_HTTP_SCHEMA_MODEL_H
#define GALAY_HTTP_SCHEMA_MODEL_H

#include "api_error.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace galay::api {

/** @brief Schema 所描述的数据方向。 */
enum class SchemaUse {
    input,  ///< 请求输入；可选字段可省略。
    output  ///< 响应输出；对象字段均要求出现。
};

/** @brief JSON Schema 数值边界，保留整数精度。 */
using SchemaNumber = std::variant<std::int64_t, std::uint64_t, double>;

/** @brief Schema 中允许的枚举值类型。 */
using SchemaValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

/**
 * @brief 可直接序列化为 OpenAPI 3.1 Schema 的数据模型。
 * @details `properties`、`items` 和 `additional_properties` 描述复合值；数值边界
 *          使用有符号整数、无符号整数或 double，避免大整数先转为 double。
 */
struct Schema {
    std::string type; ///< JSON 类型，如 `object`、`array` 或 `integer`。
    std::string format; ///< OpenAPI 格式提示，如 `int64` 或 `date-time`。
    std::string description; ///< 面向文档读者的说明。
    bool nullable = false; ///< 是否允许 JSON `null`。
    std::optional<SchemaNumber> minimum; ///< 数值最小值，包含边界。
    std::optional<SchemaNumber> maximum; ///< 数值最大值，包含边界。
    std::optional<std::size_t> min_length; ///< 字符串最小长度。
    std::optional<std::size_t> max_length; ///< 字符串最大长度。
    std::optional<std::size_t> min_items; ///< 数组最小元素数。
    std::optional<std::size_t> max_items; ///< 数组最大元素数。
    std::optional<std::size_t> min_properties; ///< 对象最少属性数。
    std::optional<std::size_t> max_properties; ///< 对象最多属性数。
    std::vector<SchemaValue> enum_values; ///< 可取的枚举值。
    std::map<std::string, Schema> properties; ///< 对象属性及其 Schema。
    std::vector<std::string> required; ///< 对象中必须出现的属性名。
    std::shared_ptr<const Schema> items; ///< 数组元素 Schema。
    std::shared_ptr<const Schema> additional_properties; ///< 附加对象属性的值 Schema。
    bool allow_additional_properties = false; ///< 是否允许未列出的对象属性。
};

/**
 * @brief 将 Schema 编码为 JSON。
 * @param schema 待验证并序列化的 Schema。
 * @return 成功时返回 JSON 文本；结构无效或编码失败时返回 ApiError。
 */
ApiResult<std::string> schema_json(const Schema& schema);

} // namespace galay::api

#endif
