/**
 * @file api_operation.h
 * @brief 类型化 API 操作、参数和响应的文档描述。
 * @details 文档模块使用这些契约验证路由并渲染 OpenAPI 3.1 文档。
 */

#ifndef GALAY_HTTP_API_OPERATION_H
#define GALAY_HTTP_API_OPERATION_H

#include "schema_model.h"
#include "../protoc/http_base.h"
#include <span>

namespace galay::api {

/** @brief HTTP 请求参数的来源。 */
enum class ParameterSource {
    path,  ///< 路由模板中的路径参数。
    query  ///< URL query 参数。
};

/** @brief OpenAPI responses 中声明的业务错误响应。 */
struct ErrorResponse {
    int status = 400; ///< HTTP 状态码。
    std::string description; ///< 响应含义。
};

/**
 * @brief 一条类型化 API 操作及其 OpenAPI 元数据。
 * @details 非 500 的业务错误状态必须列在 `errors` 中，否则运行时会将其作为未声明错误处理。
 */
struct Operation {
    std::string id; ///< OpenAPI operationId；同一文档中必须唯一。
    std::string summary; ///< 操作摘要。
    std::string description; ///< 操作详细说明。
    std::vector<std::string> tags; ///< 用于文档分组的标签。
    int success_status = 200; ///< 成功响应状态码。
    std::string success_description = "Success"; ///< 成功响应说明。
    std::vector<ErrorResponse> errors; ///< 已声明的业务错误响应。
};

/** @brief 一个 path/query 参数及其对应 DTO 字段和 Schema。 */
struct Parameter {
    ParameterSource source = ParameterSource::query; ///< 参数来源。
    std::string name; ///< 请求路径或 query 中的参数名。
    std::string field_name; ///< DTO 中对应的成员字段名。
    std::string description; ///< 参数说明。
    bool required = true; ///< 参数是否必须提供。
    Schema schema; ///< 参数值的 Schema。
};

/** @brief 一条已完成绑定和 Schema 推导的路由文档描述。 */
struct EndpointSpec {
    http::HttpMethod method = http::HttpMethod::GET; ///< HTTP 方法。
    std::string path; ///< 路由模板，如 `/users/:user_id`。
    Operation operation; ///< operationId、摘要和响应说明。
    std::vector<Parameter> parameters; ///< path/query 参数。
    std::optional<Schema> request_body; ///< JSON 请求体 Schema；无请求体时为空。
    bool body_required = false; ///< 请求体是否必填。
    std::optional<Schema> response_body; ///< JSON 响应体 Schema；无响应体时为空。
};

/** @brief OpenAPI 文档的顶层接口信息。 */
struct ApiInfo {
    std::string title = "Galay API"; ///< 文档标题。
    std::string version = "1.0.0"; ///< API 版本。
    std::string description; ///< 文档说明。
};

/** @brief 路由模板解析后的 OpenAPI 路径、冲突比较形状和参数名。 */
struct PathInfo {
    std::string openapi_path; ///< 转换为 OpenAPI 花括号参数形式的路径。
    std::string shape; ///< 参数名统一替换后的路径，用于冲突比较。
    std::vector<std::string> parameters; ///< 按路径顺序排列的参数名。
};

/** @brief 解析并校验路由模板。 */
ApiResult<PathInfo> inspect_path(std::string_view path);
/** @brief 校验操作元数据、Schema 和与既有操作的冲突。 */
ApiResult<void> validate_endpoint(const EndpointSpec& endpoint,
                                  std::span<const EndpointSpec> existing);
/** @brief 将接口信息及路由描述渲染为 OpenAPI 3.1 JSON。 */
ApiResult<std::string> render_openapi(const ApiInfo& info,
                                     std::span<const EndpointSpec> endpoints);

} // namespace galay::api

#endif
