/**
 * @file api_error.h
 * @brief 类型化 API 接口使用的错误码、错误对象和显式结果类型。
 * @details 错误码提供稳定的协议名称，ApiResult 通过 std::expected 传递成功值或失败原因。
 */

#ifndef GALAY_HTTP_API_ERROR_H
#define GALAY_HTTP_API_ERROR_H

#include <expected>
#include <string>
#include <string_view>

namespace galay::api {

/**
 * @brief API 请求、契约和响应处理中的错误类别。
 * @details 枚举值只用于 C++ 分支判断；协议响应中的稳定名称由 api_error_name 映射。
 */
enum class ApiErrorCode {
    kInvalidSchema,       ///< Schema 结构本身无效。
    kUnsupportedType,     ///< DTO 使用了不支持的 C++ 类型。
    kInvalidMetadata,     ///< 字段、操作或接口元数据不符合契约。
    kInvalidPath,         ///< 路由路径不符合支持的格式。
    kRouteConflict,        ///< 路由与已注册路由冲突。
    kDuplicateOperation,  ///< OpenAPI operationId 重复。
    kInvalidBinding,      ///< 参数绑定或 handler 签名无效。
    kFrozenBuilder,       ///< server builder 已构建，不能再修改注册项。
    kBadRequest,          ///< 请求参数或请求体无法解码或未通过校验。
    kUnsupportedMediaType, ///< 请求体媒体类型不受支持。
    kBusinessError,       ///< handler 返回的业务错误。
    kTaskError,           ///< handler 协程任务本身失败。
    kEncodingError,       ///< Schema、文档或响应编码失败。
    kTransportError,      ///< 原生传输启动或停止失败。
    kResourceError,       ///< OpenAPI 或文档 UI 资源不可用。
    kServerError          ///< server 生命周期操作不符合状态要求。
};

/**
 * @brief 将 API 错误码映射为稳定的协议名称。
 * @param code 错误类别。
 * @return 固定的小写下划线名称；未知值映射为 `invalid_error_code`。
 */
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

/**
 * @brief API 操作失败时返回的错误信息。
 * @details `status` 是 HTTP 响应状态；非 500 业务状态必须在 Operation 中声明。
 */
struct ApiError {
    ApiErrorCode code = ApiErrorCode::kBadRequest; ///< 稳定错误类别。
    std::string message;                           ///< 面向调用方的错误说明。
    int status = 400;                              ///< HTTP 响应状态码。
};

/** @brief 使用 ApiError 表示失败原因的显式结果类型。 */
template<class T>
using ApiResult = std::expected<T, ApiError>;

} // namespace galay::api

#endif
