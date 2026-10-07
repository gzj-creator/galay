#ifndef GALAY_API_ERROR_H
#define GALAY_API_ERROR_H

#include <expected>
#include <string>
#include <string_view>

namespace galay::api {

enum class ApiErrorCode {
    invalid_schema,
    unsupported_type,
    invalid_metadata,
    invalid_path,
    route_conflict,
    duplicate_operation,
    invalid_binding,
    frozen_builder,
    bad_request,
    unsupported_media_type,
    business_error,
    task_error,
    encoding_error,
    transport_error,
    resource_error,
    server_error
};

constexpr std::string_view api_error_name(ApiErrorCode code) noexcept {
    switch (code) {
    case ApiErrorCode::invalid_schema: return "invalid_schema";
    case ApiErrorCode::unsupported_type: return "unsupported_type";
    case ApiErrorCode::invalid_metadata: return "invalid_metadata";
    case ApiErrorCode::invalid_path: return "invalid_path";
    case ApiErrorCode::route_conflict: return "route_conflict";
    case ApiErrorCode::duplicate_operation: return "duplicate_operation";
    case ApiErrorCode::invalid_binding: return "invalid_binding";
    case ApiErrorCode::frozen_builder: return "frozen_builder";
    case ApiErrorCode::bad_request: return "bad_request";
    case ApiErrorCode::unsupported_media_type: return "unsupported_media_type";
    case ApiErrorCode::business_error: return "business_error";
    case ApiErrorCode::task_error: return "task_error";
    case ApiErrorCode::encoding_error: return "encoding_error";
    case ApiErrorCode::transport_error: return "transport_error";
    case ApiErrorCode::resource_error: return "resource_error";
    case ApiErrorCode::server_error: return "server_error";
    }
    return "invalid_error_code";
}

struct ApiError {
    ApiErrorCode code = ApiErrorCode::bad_request;
    std::string message;
    int status = 400;
};

template<class T>
using ApiResult = std::expected<T, ApiError>;

} // namespace galay::api

#endif
