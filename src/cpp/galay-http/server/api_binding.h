/**
 * @file api_binding.h
 * @brief 输入 DTO 的成员到请求参数绑定声明。
 * @details path/query 参数解码和 Schema 生成由可选 galay-doc 模块提供实现。
 */

#ifndef GALAY_HTTP_API_BINDING_H
#define GALAY_HTTP_API_BINDING_H

#include "api_operation.h"
#include "../protoc/http_request.h"
#include <functional>

namespace galay::api {

/** @brief 表示没有 path、query 或 JSON 请求体输入的 DTO 标记。 */
struct NoInput {};
/** @brief 表示 handler 不生成 JSON 响应体的输出标记。 */
struct NoContent {};

/** @brief 输入 DTO 与已校验参数绑定计划。 */
template<class Input>
struct BindingPlan {
    std::vector<Parameter> parameters; ///< 文档中的 path/query 参数。
    std::optional<Schema> body_schema; ///< 未绑定成员组成的 JSON 请求体 Schema。
    bool body_required = false; ///< 是否要求请求体。
    std::function<ApiResult<Input>(http::HttpRequest&)> decode; ///< 校验并构造输入 DTO。
};

/**
 * @brief 将输入 DTO 的成员绑定到 path/query 参数，其余成员作为 JSON 请求体。
 * @tparam Input serde 静态反射可描述的输入 DTO。
 */
template<class Input>
class InputBinding {
public:
    /** @brief 将 DTO 成员绑定到路径参数。 */
    template<auto Member>
    InputBinding& path(std::string name);
    /** @brief 将 DTO 成员绑定到 query 参数。 */
    template<auto Member>
    InputBinding& query(std::string name);
    /**
     * @brief 根据 HTTP 方法和路由模板校验绑定并生成解码计划。
     * @param method 注册此操作使用的 HTTP 方法。
     * @param path 路由模板；`:name` 表示路径参数。
     * @return 参数 Schema 与请求解码器；绑定不一致时返回 ApiError。
     */
    ApiResult<BindingPlan<Input>> prepare(http::HttpMethod method,
                                         std::string_view path) const;

private:
    struct MemberBinding {
        ParameterSource source;
        std::string name;
        std::string field_name;
        std::function<ApiResult<Parameter>()> describe;
        std::function<ApiResult<void>(Input&, std::optional<std::string_view>)> decode;
    };
    template<auto Member>
    InputBinding& add(ParameterSource source, std::string name);
    std::vector<MemberBinding> members_;
    std::optional<ApiError> error_;
};

} // namespace galay::api

#endif
