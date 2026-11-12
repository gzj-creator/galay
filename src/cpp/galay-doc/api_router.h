/**
 * @file api_router.h
 * @brief 为原生 HTTP server builder 实现类型化 API 请求处理。
 * @details add_api 根据输入 DTO 生成绑定、校验和 OpenAPI 操作描述；启用文档时，
 *          builder 会在 build() 前生成并挂载 OpenAPI JSON 与 Swagger UI。
 */

#ifndef GALAY_DOC_ROUTER_H
#define GALAY_DOC_ROUTER_H

#include <galay/cpp/galay-http/server/api_contract.h>
#include "binding.h"
#include <galay/cpp/galay-http/server/server_routes.h>

#include <functional>
#include <type_traits>

namespace galay::api {
namespace router_detail {

ApiResult<std::string> encode_error(const ApiError& error);
http::HttpResponseResult make_response(int status, std::string body, bool json_body, bool head);

template<class Input, class Handler>
struct RouteState {
    Handler handler;
    std::function<ApiResult<Input>(http::HttpRequest&)> decode;
    std::string operation_id;
    int success_status;
    std::vector<int> error_statuses;
};

// 按值接收协程参数，使请求和路由状态由协程帧持有，不依赖临时 lambda 闭包的生命周期。
template<class Input, class Output, class Handler>
kernel::Task<http::HttpResponseResult> execute_route(std::shared_ptr<RouteState<Input, Handler>> state,
                                                   http::HttpRequest request, bool head) {
    int status = state->success_status;
    std::string body;
    bool json_body = false;
    std::optional<ApiError> failure;
    auto input = state->decode(request);
    if (!input) {
        failure = std::move(input.error());
    } else {
        ApiContext context{request};
        auto task_result = co_await std::invoke(state->handler, context, std::move(*input));
        if (!task_result) {
            failure = ApiError{ApiErrorCode::kTaskError, std::string(task_result.error().message()), 500};
            HTTP_LOG_ERROR("[api] [handler-task-fail]", "operation={} error={}", state->operation_id, failure->message);
        } else if (!*task_result) {
            failure = std::move(task_result->error());
            const bool declared = std::find(state->error_statuses.begin(), state->error_statuses.end(),
                failure->status) != state->error_statuses.end();
            if (failure->status != 500 && !declared) {
                HTTP_LOG_WARN("[api] [undeclared-business-status]", "operation={} status={}",
                              state->operation_id, failure->status);
                failure->status = 500;
            }
        } else if constexpr (!std::same_as<Output, NoContent>) {
            if (auto checked = validate_contract(**task_result); !checked) {
                failure = ApiError{ApiErrorCode::kEncodingError, std::move(checked.error().message), 500};
                HTTP_LOG_ERROR("[api] [contract-fail]", "operation={} error={}", state->operation_id, failure->message);
            } else if (!head) {
                auto encoded = json::serialize(**task_result);
                if (!encoded) {
                    failure = ApiError{ApiErrorCode::kEncodingError, std::move(encoded.error()), 500};
                    HTTP_LOG_ERROR("[api] [encode-fail]", "operation={} error={}", state->operation_id, failure->message);
                } else {
                    body = std::move(*encoded);
                    json_body = true;
                }
            }
        }
    }
    if (failure) {
        status = failure->status;
        if (!head) {
            auto encoded = encode_error(*failure);
            if (!encoded) {
                HTTP_LOG_ERROR("[api] [error-encode-fail]", "operation={} error={}",
                              state->operation_id, encoded.error().message);
                status = 500;
                encoded = encode_error(ApiError{ApiErrorCode::kEncodingError, "API error response could not be encoded", 500});
                if (!encoded) {
                    HTTP_LOG_ERROR("[api] [error-encode-fail]", "operation={} error={}",
                                  state->operation_id, encoded.error().message);
                    co_return std::unexpected(http::HttpError{http::kInternalError, encoded.error().message});
                }
            }
            body = std::move(*encoded);
            json_body = true;
        }
    }

    co_return make_response(status, std::move(body), json_body, head);
}

} // namespace router_detail
} // namespace galay::api

namespace galay::http::server_detail {

using namespace galay::api;

template<bool EnableDocs>
template<http::HttpMethod Method, class Input, class Output, class Handler>
ApiResult<void> ServerRoutes<EnableDocs>::add_api(std::string path, Handler handler, Operation operation,
                                InputBinding<Input> binding) {
    if (built_) return std::unexpected(ApiError{ApiErrorCode::kFrozenBuilder, "server builder has already been built", 500});
    if constexpr (!std::is_invocable_r_v<kernel::Task<ApiResult<Output>>, Handler&, ApiContext&, Input>) {
        return std::unexpected(ApiError{ApiErrorCode::kInvalidBinding, "handler must return Task<ApiResult<Output>>", 500});
    } else {
        auto plan = binding.prepare(Method, path);
        if (!plan) return std::unexpected(std::move(plan.error()));
        EndpointSpec endpoint{Method, path, operation, plan->parameters, plan->body_schema, plan->body_required, {}};
        if constexpr (!std::same_as<Output, NoContent>) {
            if (operation.success_status == 204 || operation.success_status == 205) {
                return std::unexpected(ApiError{ApiErrorCode::kInvalidMetadata, "204/205 responses require NoContent output", 500});
            }
            auto response = schema_for<Output>(SchemaUse::output);
            if (!response) return std::unexpected(std::move(response.error()));
            if constexpr (Method != http::HttpMethod::HEAD) endpoint.response_body = std::move(*response);
        }
        if (auto checked = validate_endpoint(endpoint, {}); !checked) return checked;
        RouteKey key{Method, path, operation.id};
        if (auto checked = check_route(key); !checked) return checked;

        std::vector<int> error_statuses;
        for (const auto& error : operation.errors) error_statuses.push_back(error.status);

        auto state = std::make_shared<router_detail::RouteState<Input, Handler>>(
            router_detail::RouteState<Input, Handler>{std::move(handler), std::move(plan->decode),
                operation.id, operation.success_status, std::move(error_statuses)});
        http::HttpRequestHandler route = [state = std::move(state)](http::HttpRequest request) {
            return router_detail::execute_route<Input, Output>(state, std::move(request), Method == http::HttpMethod::HEAD);
        };
        if constexpr (EnableDocs) docs_.endpoints.push_back(std::move(endpoint));
        routes_.push_back({std::move(key), http::HttpRouteEntry{{}, std::move(route)}});
        return {};
    }
}

} // namespace galay::http::server_detail

#endif
