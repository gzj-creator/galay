#ifndef GALAY_HTTP_API_ERROR_H
#define GALAY_HTTP_API_ERROR_H

#include <expected>
#include <string>
#include <string_view>

namespace galay::api {

enum class ApiErrorCode {
    kInvalidSchema,
    kUnsupportedType,
    kInvalidMetadata,
    kInvalidPath,
    kRouteConflict,
    kDuplicateOperation,
    kInvalidBinding,
    kFrozenBuilder,
    kBadRequest,
    kUnsupportedMediaType,
    kBusinessError,
    kTaskError,
    kEncodingError,
    kTransportError,
    kResourceError,
    kServerError
};

constexpr std::string_view api_error_name(ApiErrorCode code) noexcept {
    switch (code) {
    case ApiErrorCode::kInvalidSchema: return "invalid_schema";
    case ApiErrorCode::kUnsupportedType: return "unsupported_type";
    case ApiErrorCode::kInvalidMetadata: return "invalid_metadata";
    case ApiErrorCode::kInvalidPath: return "invalid_path";
    case ApiErrorCode::kRouteConflict: return "route_conflict";
    case ApiErrorCode::kDuplicateOperation: return "duplicate_operation";
    case ApiErrorCode::kInvalidBinding: return "invalid_binding";
    case ApiErrorCode::kFrozenBuilder: return "frozen_builder";
    case ApiErrorCode::kBadRequest: return "bad_request";
    case ApiErrorCode::kUnsupportedMediaType: return "unsupported_media_type";
    case ApiErrorCode::kBusinessError: return "business_error";
    case ApiErrorCode::kTaskError: return "task_error";
    case ApiErrorCode::kEncodingError: return "encoding_error";
    case ApiErrorCode::kTransportError: return "transport_error";
    case ApiErrorCode::kResourceError: return "resource_error";
    case ApiErrorCode::kServerError: return "server_error";
    }
    return "invalid_error_code";
}

struct ApiError {
    ApiErrorCode code = ApiErrorCode::kBadRequest;
    std::string message;
    int status = 400;
};

template<class T>
using ApiResult = std::expected<T, ApiError>;

} // namespace galay::api

#endif
