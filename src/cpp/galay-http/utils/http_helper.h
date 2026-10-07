/**
 * @file http_helper.h
 * @brief HTTP 工具类，提供默认请求和响应的快速构造
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HTTP 各方法（GET/POST/PUT/DELETE 等）的默认请求构造，
 * 以及所有标准 HTTP 状态码对应的默认 HTML 响应构造。
 * 同时提供 WebSocket 升级握手相关的辅助方法。
 *
 * 使用方式：
 * @code
 * // 构造默认 GET 请求
 * auto req = HttpHelper::default_get("/api/users");
 *
 * // 构造默认 404 响应
 * auto rsp = HttpHelper::default_not_found();
 *
 * // 根据状态码自动选择默认响应
 * auto rsp = HttpHelper::default_http_response(HttpStatusCode::BadRequest_400);
 * @endcode
 */

#ifndef GALAY_HTTP_HELPER_H
#define GALAY_HTTP_HELPER_H

#include "../protoc/http_request.h"
#include "../protoc/http_response.h"

namespace galay::http
{
/**
 * @brief HTTP 工具类
 * @details 静态工具类，提供各种 HTTP 请求和响应的快速构造方法。
 * 所有方法均为静态方法，无需实例化。
 * 包含 HTTP 标准方法（GET/POST/PUT/DELETE/PATCH/HEAD/OPTIONS）的默认请求构造，
 * 所有 RFC 7231/6585/4918/7538 等定义的标准状态码默认响应，
 * 以及 WebSocket 升级握手的辅助方法。
 */
class HttpHelper {
public:
    // ==================== HTTP 请求方法 ====================

    /**
     * @brief 构造默认 GET 请求
     * @param[in] uri 请求 URI
     * @return 构造好的 HttpRequest 对象
     * @note 自动设置 User-Agent 和 Accept 头
     */
    static HttpRequest default_get(std::string_view uri);

    /**
     * @brief 构造默认 POST 请求
     * @param[in] uri 请求 URI
     * @param[in] body 请求体内容（默认为空）
     * @return 构造好的 HttpRequest 对象
     * @note 当 body 非空时自动设置 Content-Type 为 application/x-www-form-urlencoded
     */
    static HttpRequest default_post(std::string_view uri, std::string&& body = "");

    /**
     * @brief 构造默认 PUT 请求
     * @param[in] uri 请求 URI
     * @param[in] body 请求体内容（默认为空）
     * @return 构造好的 HttpRequest 对象
     * @note 当 body 非空时自动设置 Content-Type 为 application/x-www-form-urlencoded
     */
    static HttpRequest default_put(std::string_view uri, std::string&& body = "");

    /**
     * @brief 构造默认 DELETE 请求
     * @param[in] uri 请求 URI
     * @return 构造好的 HttpRequest 对象
     */
    static HttpRequest default_delete(std::string_view uri);

    /**
     * @brief 构造默认 PATCH 请求
     * @param[in] uri 请求 URI
     * @param[in] body 请求体内容（默认为空）
     * @return 构造好的 HttpRequest 对象
     * @note 当 body 非空时自动设置 Content-Type 为 application/x-www-form-urlencoded
     */
    static HttpRequest default_patch(std::string_view uri, std::string&& body = "");

    /**
     * @brief 构造默认 HEAD 请求
     * @param[in] uri 请求 URI
     * @return 构造好的 HttpRequest 对象
     */
    static HttpRequest default_head(std::string_view uri);

    /**
     * @brief 构造默认 OPTIONS 请求
     * @param[in] uri 请求 URI
     * @return 构造好的 HttpRequest 对象
     */
    static HttpRequest default_options(std::string_view uri);

    // ==================== 1xx 信息性响应 ====================

    /**
     * @brief 构造默认 100 Continue 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_continue();

    /**
     * @brief 构造默认 101 Switching Protocol 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_switching_protocol();

    /**
     * @brief 构造默认 102 Processing 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_processing();

    /**
     * @brief 构造默认 103 Early Hints 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_early_hints();

    // ==================== 2xx 成功响应 ====================

    /**
     * @brief 构造默认 200 OK 响应
     * @param[in] type 内容 MIME 类型（如 "html"、"json"、"text"）
     * @param[in] body 响应体内容
     * @return 构造好的 HttpResponse 对象
     * @note 自动通过 MimeType 将类型字符串转换为标准 MIME 类型
     */
    static HttpResponse default_ok(const std::string& type, std::string&& body);

    /**
     * @brief 构造默认 201 Created 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_created();

    /**
     * @brief 构造默认 202 Accepted 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_accepted();

    /**
     * @brief 构造默认 203 Non-Authoritative Information 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_non_authoritative_information();

    /**
     * @brief 构造默认 204 No Content 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_no_content();

    /**
     * @brief 构造默认 205 Reset Content 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_reset_content();

    /**
     * @brief 构造默认 206 Partial Content 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_partial_content();

    /**
     * @brief 构造默认 207 Multi-Status 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_multi_status();

    /**
     * @brief 构造默认 208 Already Reported 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_already_reported();

    /**
     * @brief 构造默认 226 IM Used 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_im_used();

    // ==================== 3xx 重定向响应 ====================

    /**
     * @brief 构造默认 300 Multiple Choices 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_multiple_choices();

    /**
     * @brief 构造默认 301 Moved Permanently 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_moved_permanently();

    /**
     * @brief 构造默认 302 Found 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_found();

    /**
     * @brief 构造默认 303 See Other 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_see_other();

    /**
     * @brief 构造默认 304 Not Modified 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_not_modified();

    /**
     * @brief 构造默认 305 Use Proxy 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_use_proxy();

    /**
     * @brief 构造默认 306 Unused 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_unused();

    /**
     * @brief 构造默认 307 Temporary Redirect 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_temporary_redirect();

    /**
     * @brief 构造默认 308 Permanent Redirect 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_permanent_redirect();

    // ==================== 4xx 客户端错误响应 ====================

    /**
     * @brief 构造默认 400 Bad Request 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_bad_request();

    /**
     * @brief 构造默认 401 Unauthorized 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_unauthorized();

    /**
     * @brief 构造默认 402 Payment Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_payment_required();

    /**
     * @brief 构造默认 403 Forbidden 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_forbidden();

    /**
     * @brief 构造默认 404 Not Found 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_not_found();

    /**
     * @brief 构造默认 405 Method Not Allowed 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_method_not_allowed();

    /**
     * @brief 构造默认 406 Not Acceptable 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_not_acceptable();

    /**
     * @brief 构造默认 407 Proxy Authentication Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_proxy_authentication_required();

    /**
     * @brief 构造默认 408 Request Timeout 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_request_timeout();

    /**
     * @brief 构造默认 409 Conflict 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_conflict();

    /**
     * @brief 构造默认 410 Gone 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_gone();

    /**
     * @brief 构造默认 411 Length Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_length_required();

    /**
     * @brief 构造默认 412 Precondition Failed 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_precondition_failed();

    /**
     * @brief 构造默认 413 Payload Too Large 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_payload_too_large();

    /**
     * @brief 构造默认 414 URI Too Long 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_uri_too_long();

    /**
     * @brief 构造默认 415 Unsupported Media Type 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_unsupported_media_type();

    /**
     * @brief 构造默认 416 Range Not Satisfiable 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_range_not_satisfiable();

    /**
     * @brief 构造默认 417 Expectation Failed 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_expectation_failed();

    /**
     * @brief 构造默认 418 I'm a teapot 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_im_a_teapot();

    /**
     * @brief 构造默认 421 Misdirected Request 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_misdirected_request();

    /**
     * @brief 构造默认 422 Unprocessable Content 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_unprocessable_content();

    /**
     * @brief 构造默认 423 Locked 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_locked();

    /**
     * @brief 构造默认 424 Failed Dependency 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_failed_dependency();

    /**
     * @brief 构造默认 425 Too Early 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_too_early();

    /**
     * @brief 构造默认 426 Upgrade Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_upgrade_required();

    /**
     * @brief 构造默认 428 Precondition Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_precondition_required();

    /**
     * @brief 构造默认 429 Too Many Requests 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_too_many_requests();

    /**
     * @brief 构造默认 431 Request Header Fields Too Large 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_request_header_fields_too_large();

    /**
     * @brief 构造默认 451 Unavailable For Legal Reasons 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_unavailable_for_legal_reasons();

    // ==================== 5xx 服务端错误响应 ====================

    /**
     * @brief 构造默认 500 Internal Server Error 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_internal_server_error();

    /**
     * @brief 构造默认 501 Not Implemented 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_not_implemented();

    /**
     * @brief 构造默认 502 Bad Gateway 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_bad_gateway();

    /**
     * @brief 构造默认 503 Service Unavailable 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_service_unavailable();

    /**
     * @brief 构造默认 504 Gateway Timeout 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_gateway_timeout();

    /**
     * @brief 构造默认 505 HTTP Version Not Supported 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_http_version_not_supported();

    /**
     * @brief 构造默认 506 Variant Also Negotiates 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_variant_also_negotiates();

    /**
     * @brief 构造默认 507 Insufficient Storage 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_insufficient_storage();

    /**
     * @brief 构造默认 508 Loop Detected 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_loop_detected();

    /**
     * @brief 构造默认 510 Not Extended 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_not_extended();

    /**
     * @brief 构造默认 511 Network Authentication Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse default_network_authentication_required();

    // ==================== 通用方法 ====================

    /**
     * @brief 根据状态码枚举值构造对应的默认 HTTP 响应
     * @param[in] code HTTP 状态码枚举
     * @return 对应状态码的默认 HttpResponse 对象
     * @details 内部通过 switch-case 分发到各个 default* 方法，
     *          若状态码不在已知范围内，返回 500 Internal Server Error
     */
    static HttpResponse default_http_response(HttpStatusCode code);

    // ==================== WebSocket 相关 ====================

    /**
     * @brief 构造 WebSocket 协议升级响应
     * @param[in] clientKey 客户端发送的 Sec-WebSocket-Key 头部值
     * @return 构造好的 HttpResponse 对象
     * @details 当启用 ENABLE_WEBSOCKET 时，返回 101 Switching Protocol 响应，
     *          包含 Upgrade、Connection、Sec-WebSocket-Accept 等头部。
     *          当未启用 WebSocket 时，返回 501 Not Implemented。
     */
    static HttpResponse create_web_socket_upgrade_response(const std::string& clientKey);

private:
    /**
     * @brief 生成 WebSocket Accept Key
     * @param[in] clientKey 客户端的 Sec-WebSocket-Key
     * @return 经过 SHA-1 哈希并 Base64 编码后的服务端 accept key
     * @details 按照 RFC 6455 规范，将 clientKey 与固定的 magic string 拼接后
     *          进行 SHA-1 哈希，再进行 Base64 编码。
     *          当未启用 ENABLE_WEBSOCKET 时返回空字符串。
     */
    static std::string generate_web_socket_accept_key(const std::string& clientKey);
};
}

#endif
