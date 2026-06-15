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
 * auto req = HttpHelper::defaultGet("/api/users");
 *
 * // 构造默认 404 响应
 * auto rsp = HttpHelper::defaultNotFound();
 *
 * // 根据状态码自动选择默认响应
 * auto rsp = HttpHelper::defaultHttpResponse(HttpStatusCode::BadRequest_400);
 * @endcode
 */

#ifndef GALAY_HTTP_HELPER_H
#define GALAY_HTTP_HELPER_H

#include "galay-http/protoc/http_request.h"
#include "galay-http/protoc/http_response.h"

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
    static HttpRequest defaultGet(std::string_view uri);

    /**
     * @brief 构造默认 POST 请求
     * @param[in] uri 请求 URI
     * @param[in] body 请求体内容（默认为空）
     * @return 构造好的 HttpRequest 对象
     * @note 当 body 非空时自动设置 Content-Type 为 application/x-www-form-urlencoded
     */
    static HttpRequest defaultPost(std::string_view uri, std::string&& body = "");

    /**
     * @brief 构造默认 PUT 请求
     * @param[in] uri 请求 URI
     * @param[in] body 请求体内容（默认为空）
     * @return 构造好的 HttpRequest 对象
     * @note 当 body 非空时自动设置 Content-Type 为 application/x-www-form-urlencoded
     */
    static HttpRequest defaultPut(std::string_view uri, std::string&& body = "");

    /**
     * @brief 构造默认 DELETE 请求
     * @param[in] uri 请求 URI
     * @return 构造好的 HttpRequest 对象
     */
    static HttpRequest defaultDelete(std::string_view uri);

    /**
     * @brief 构造默认 PATCH 请求
     * @param[in] uri 请求 URI
     * @param[in] body 请求体内容（默认为空）
     * @return 构造好的 HttpRequest 对象
     * @note 当 body 非空时自动设置 Content-Type 为 application/x-www-form-urlencoded
     */
    static HttpRequest defaultPatch(std::string_view uri, std::string&& body = "");

    /**
     * @brief 构造默认 HEAD 请求
     * @param[in] uri 请求 URI
     * @return 构造好的 HttpRequest 对象
     */
    static HttpRequest defaultHead(std::string_view uri);

    /**
     * @brief 构造默认 OPTIONS 请求
     * @param[in] uri 请求 URI
     * @return 构造好的 HttpRequest 对象
     */
    static HttpRequest defaultOptions(std::string_view uri);

    // ==================== 1xx 信息性响应 ====================

    /**
     * @brief 构造默认 100 Continue 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultContinue();

    /**
     * @brief 构造默认 101 Switching Protocol 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultSwitchingProtocol();

    /**
     * @brief 构造默认 102 Processing 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultProcessing();

    /**
     * @brief 构造默认 103 Early Hints 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultEarlyHints();

    // ==================== 2xx 成功响应 ====================

    /**
     * @brief 构造默认 200 OK 响应
     * @param[in] type 内容 MIME 类型（如 "html"、"json"、"text"）
     * @param[in] body 响应体内容
     * @return 构造好的 HttpResponse 对象
     * @note 自动通过 MimeType 将类型字符串转换为标准 MIME 类型
     */
    static HttpResponse defaultOk(const std::string& type, std::string&& body);

    /**
     * @brief 构造默认 201 Created 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultCreated();

    /**
     * @brief 构造默认 202 Accepted 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultAccepted();

    /**
     * @brief 构造默认 203 Non-Authoritative Information 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultNonAuthoritativeInformation();

    /**
     * @brief 构造默认 204 No Content 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultNoContent();

    /**
     * @brief 构造默认 205 Reset Content 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultResetContent();

    /**
     * @brief 构造默认 206 Partial Content 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultPartialContent();

    /**
     * @brief 构造默认 207 Multi-Status 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultMultiStatus();

    /**
     * @brief 构造默认 208 Already Reported 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultAlreadyReported();

    /**
     * @brief 构造默认 226 IM Used 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultIMUsed();

    // ==================== 3xx 重定向响应 ====================

    /**
     * @brief 构造默认 300 Multiple Choices 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultMultipleChoices();

    /**
     * @brief 构造默认 301 Moved Permanently 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultMovedPermanently();

    /**
     * @brief 构造默认 302 Found 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultFound();

    /**
     * @brief 构造默认 303 See Other 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultSeeOther();

    /**
     * @brief 构造默认 304 Not Modified 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultNotModified();

    /**
     * @brief 构造默认 305 Use Proxy 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultUseProxy();

    /**
     * @brief 构造默认 306 Unused 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultUnused();

    /**
     * @brief 构造默认 307 Temporary Redirect 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultTemporaryRedirect();

    /**
     * @brief 构造默认 308 Permanent Redirect 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultPermanentRedirect();

    // ==================== 4xx 客户端错误响应 ====================

    /**
     * @brief 构造默认 400 Bad Request 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultBadRequest();

    /**
     * @brief 构造默认 401 Unauthorized 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultUnauthorized();

    /**
     * @brief 构造默认 402 Payment Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultPaymentRequired();

    /**
     * @brief 构造默认 403 Forbidden 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultForbidden();

    /**
     * @brief 构造默认 404 Not Found 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultNotFound();

    /**
     * @brief 构造默认 405 Method Not Allowed 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultMethodNotAllowed();

    /**
     * @brief 构造默认 406 Not Acceptable 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultNotAcceptable();

    /**
     * @brief 构造默认 407 Proxy Authentication Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultProxyAuthenticationRequired();

    /**
     * @brief 构造默认 408 Request Timeout 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultRequestTimeout();

    /**
     * @brief 构造默认 409 Conflict 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultConflict();

    /**
     * @brief 构造默认 410 Gone 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultGone();

    /**
     * @brief 构造默认 411 Length Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultLengthRequired();

    /**
     * @brief 构造默认 412 Precondition Failed 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultPreconditionFailed();

    /**
     * @brief 构造默认 413 Payload Too Large 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultPayloadTooLarge();

    /**
     * @brief 构造默认 414 URI Too Long 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultUriTooLong();

    /**
     * @brief 构造默认 415 Unsupported Media Type 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultUnsupportedMediaType();

    /**
     * @brief 构造默认 416 Range Not Satisfiable 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultRangeNotSatisfiable();

    /**
     * @brief 构造默认 417 Expectation Failed 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultExpectationFailed();

    /**
     * @brief 构造默认 418 I'm a teapot 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultImATeapot();

    /**
     * @brief 构造默认 421 Misdirected Request 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultMisdirectedRequest();

    /**
     * @brief 构造默认 422 Unprocessable Content 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultUnprocessableContent();

    /**
     * @brief 构造默认 423 Locked 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultLocked();

    /**
     * @brief 构造默认 424 Failed Dependency 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultFailedDependency();

    /**
     * @brief 构造默认 425 Too Early 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultTooEarly();

    /**
     * @brief 构造默认 426 Upgrade Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultUpgradeRequired();

    /**
     * @brief 构造默认 428 Precondition Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultPreconditionRequired();

    /**
     * @brief 构造默认 429 Too Many Requests 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultTooManyRequests();

    /**
     * @brief 构造默认 431 Request Header Fields Too Large 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultRequestHeaderFieldsTooLarge();

    /**
     * @brief 构造默认 451 Unavailable For Legal Reasons 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultUnavailableForLegalReasons();

    // ==================== 5xx 服务端错误响应 ====================

    /**
     * @brief 构造默认 500 Internal Server Error 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultInternalServerError();

    /**
     * @brief 构造默认 501 Not Implemented 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultNotImplemented();

    /**
     * @brief 构造默认 502 Bad Gateway 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultBadGateway();

    /**
     * @brief 构造默认 503 Service Unavailable 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultServiceUnavailable();

    /**
     * @brief 构造默认 504 Gateway Timeout 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultGatewayTimeout();

    /**
     * @brief 构造默认 505 HTTP Version Not Supported 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultHttpVersionNotSupported();

    /**
     * @brief 构造默认 506 Variant Also Negotiates 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultVariantAlsoNegotiates();

    /**
     * @brief 构造默认 507 Insufficient Storage 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultInsufficientStorage();

    /**
     * @brief 构造默认 508 Loop Detected 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultLoopDetected();

    /**
     * @brief 构造默认 510 Not Extended 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultNotExtended();

    /**
     * @brief 构造默认 511 Network Authentication Required 响应
     * @return 构造好的 HttpResponse 对象
     */
    static HttpResponse defaultNetworkAuthenticationRequired();

    // ==================== 通用方法 ====================

    /**
     * @brief 根据状态码枚举值构造对应的默认 HTTP 响应
     * @param[in] code HTTP 状态码枚举
     * @return 对应状态码的默认 HttpResponse 对象
     * @details 内部通过 switch-case 分发到各个 default* 方法，
     *          若状态码不在已知范围内，返回 500 Internal Server Error
     */
    static HttpResponse defaultHttpResponse(HttpStatusCode code);

    // ==================== WebSocket 相关 ====================

    /**
     * @brief 构造 WebSocket 协议升级响应
     * @param[in] clientKey 客户端发送的 Sec-WebSocket-Key 头部值
     * @return 构造好的 HttpResponse 对象
     * @details 当启用 ENABLE_WEBSOCKET 时，返回 101 Switching Protocol 响应，
     *          包含 Upgrade、Connection、Sec-WebSocket-Accept 等头部。
     *          当未启用 WebSocket 时，返回 501 Not Implemented。
     */
    static HttpResponse createWebSocketUpgradeResponse(const std::string& clientKey);

private:
    /**
     * @brief 生成 WebSocket Accept Key
     * @param[in] clientKey 客户端的 Sec-WebSocket-Key
     * @return 经过 SHA-1 哈希并 Base64 编码后的服务端 accept key
     * @details 按照 RFC 6455 规范，将 clientKey 与固定的 magic string 拼接后
     *          进行 SHA-1 哈希，再进行 Base64 编码。
     *          当未启用 ENABLE_WEBSOCKET 时返回空字符串。
     */
    static std::string generateWebSocketAcceptKey(const std::string& clientKey);
};
}

#endif
