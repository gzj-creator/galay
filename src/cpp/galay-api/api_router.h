#ifndef GALAY_API_ROUTER_H
#define GALAY_API_ROUTER_H

#include "api_contract.h"
#include "binding.h"

#include <functional>
#include <type_traits>

namespace galay::api {
namespace router_detail {

ApiResult<std::string> encode_error(const ApiError& error);
http::HttpResponseResult make_response(int status, std::string body, bool json_body, bool head);

template<class Input, class Handler>
struct RouteState {
    Handler handler;
    BindingPlan<Input> binding;
    Operation operation;
};

// By-value parameters belong to the coroutine frame, not to a temporary lambda closure.
template<class Input, class Output, class Handler>
kernel::Task<http::HttpResponseResult> execute_route(std::shared_ptr<RouteState<Input, Handler>> state,
                                                   http::HttpRequest request, bool head) {
    int status = state->operation.success_status;
    std::string body;
    bool json_body = false;
    std::optional<ApiError> failure;
    auto input = state->binding.decode(request);
    if (!input) {
        failure = std::move(input.error());
    } else {
        ApiContext context{request};
        auto task_result = co_await std::invoke(state->handler, context, std::move(*input));
        if (!task_result) {
            failure = ApiError{ApiErrorCode::kTaskError, std::string(task_result.error().message()), 500};
            HTTP_LOG_ERROR("[api] [handler-task-fail]", "operation={} error={}", state->operation.id, failure->message);
        } else if (!*task_result) {
            failure = std::move(task_result->error());
            const bool declared = std::any_of(state->operation.errors.begin(), state->operation.errors.end(),
                [&failure](const auto& response) { return response.status == failure->status; });
            if (failure->status != 500 && !declared) {
                HTTP_LOG_WARN("[api] [undeclared-business-status]", "operation={} status={}",
                              state->operation.id, failure->status);
                failure->status = 500;
            }
        } else if constexpr (!std::same_as<Output, NoContent>) {
            if (auto checked = validate_contract(**task_result); !checked) {
                failure = ApiError{ApiErrorCode::kEncodingError, std::move(checked.error().message), 500};
                HTTP_LOG_ERROR("[api] [contract-fail]", "operation={} error={}", state->operation.id, failure->message);
            } else if (!head) {
                auto encoded = json::serialize(**task_result);
                if (!encoded) {
                    failure = ApiError{ApiErrorCode::kEncodingError, std::move(encoded.error()), 500};
                    HTTP_LOG_ERROR("[api] [encode-fail]", "operation={} error={}", state->operation.id, failure->message);
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
                              state->operation.id, encoded.error().message);
                status = 500;
                encoded = encode_error(ApiError{ApiErrorCode::kEncodingError, "API error response could not be encoded", 500});
                if (!encoded) {
                    HTTP_LOG_ERROR("[api] [error-encode-fail]", "operation={} error={}",
                                  state->operation.id, encoded.error().message);
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

template<http::HttpMethod Method, class Input, class Output, class Handler>
ApiResult<void> ApiBuilder::add(std::string path, Handler handler, Operation operation,
                                InputBinding<Input> binding) {
    if (built_) return std::unexpected(ApiError{ApiErrorCode::kFrozenBuilder, "API builder has already been built", 500});
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
        if (auto checked = validate_endpoint(endpoint, endpoints_); !checked) return checked;

        auto state = std::make_shared<router_detail::RouteState<Input, Handler>>(
            router_detail::RouteState<Input, Handler>{std::move(handler), std::move(*plan), std::move(operation)});
        http::HttpRequestHandler route = [state = std::move(state)](http::HttpRequest request) {
            return router_detail::execute_route<Input, Output>(state, std::move(request), Method == http::HttpMethod::HEAD);
        };
        endpoints_.push_back(std::move(endpoint));
        routes_.push_back(RegisteredRoute{Method, std::move(path), std::move(route)});
        return {};
    }
}

} // namespace galay::api

#endif
