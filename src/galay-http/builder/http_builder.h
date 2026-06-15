/**
 * @file http_builder.h
 * @brief HTTP/1.1 request and response builders.
 */

#ifndef GALAY_HTTP_BUILDER_H
#define GALAY_HTTP_BUILDER_H

#include "galay-http/protoc/http_base.h"
#include "galay-http/protoc/http_header.h"
#include "galay-http/protoc/http_request.h"
#include "galay-http/protoc/http_response.h"

#include <map>
#include <string>

namespace galay::http
{

/**
 * @brief HTTP/1.1 请求构造器
 * @details 提供链式调用接口来构造 HTTP 请求，简化代码。
 */
class Http1_1RequestBuilder
{
public:
    /**
     * @brief 构造函数
     * @param mode Header 归一化策略（默认 ClientSide，适合 Client 端）
     */
    explicit Http1_1RequestBuilder(HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);

    /**
     * @brief 设置 HTTP 方法
     * @param method HTTP 方法
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& method(HttpMethod method);

    /**
     * @brief 设置请求 URI
     * @param uri 请求 URI
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& uri(const std::string& uri);

    /**
     * @brief 添加请求头
     * @param key 头部键
     * @param value 头部值
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& header(const std::string& key, const std::string& value);

    /**
     * @brief 批量添加请求头
     * @param headers 头部键值对
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& headers(const std::map<std::string, std::string>& headers);

    /**
     * @brief 设置 Host 头
     * @param host 主机名
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& host(const std::string& host);

    /**
     * @brief 设置 Content-Type
     * @param contentType 内容类型
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& contentType(const std::string& contentType);

    /**
     * @brief 设置 User-Agent
     * @param userAgent 用户代理
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& userAgent(const std::string& userAgent);

    /**
     * @brief 设置 Connection 头
     * @param connection 连接类型（如 "keep-alive" 或 "close"）
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& connection(const std::string& connection);

    /**
     * @brief 设置请求体
     * @param body 请求体内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& body(const std::string& body);

    /**
     * @brief 设置请求体（移动语义）
     * @param body 请求体内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& body(std::string&& body);

    /**
     * @brief 设置 JSON 请求体（自动设置 Content-Type）
     * @param json JSON 字符串
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& json(const std::string& json);

    /**
     * @brief 设置表单请求体（自动设置 Content-Type）
     * @param form 表单数据
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& form(const std::map<std::string, std::string>& form);

    /**
     * @brief 构建 HttpRequest 对象
     * @return HttpRequest 对象
     */
    HttpRequest build();

    /**
     * @brief 构建 HttpRequest 对象（移动语义）
     * @return HttpRequest 对象
     */
    HttpRequest buildMove();

    static Http1_1RequestBuilder get(const std::string& uri, HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);
    static Http1_1RequestBuilder post(const std::string& uri, HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);
    static Http1_1RequestBuilder put(const std::string& uri, HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);
    static Http1_1RequestBuilder del(const std::string& uri, HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);
    static Http1_1RequestBuilder patch(const std::string& uri, HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);
    static Http1_1RequestBuilder head(const std::string& uri, HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);
    static Http1_1RequestBuilder options(const std::string& uri, HeaderPair::Mode mode = HeaderPair::Mode::ClientSide);

private:
    HttpRequest m_request;
    std::string m_body;
};

/**
 * @brief HTTP/1.1 响应构造器
 * @details 提供链式调用接口来构造 HTTP 响应，简化代码。
 */
class Http1_1ResponseBuilder
{
public:
    /**
     * @brief 构造函数
     */
    Http1_1ResponseBuilder();

    /**
     * @brief 设置状态码
     * @param code HTTP 状态码
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& status(int code);

    /**
     * @brief 设置状态码（使用枚举）
     * @param code HTTP 状态码枚举
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& status(HttpStatusCode code);

    /**
     * @brief 添加响应头
     * @param key 头部键
     * @param value 头部值
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& header(const std::string& key, const std::string& value);

    /**
     * @brief 批量添加响应头
     * @param headers 头部键值对
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& headers(const std::map<std::string, std::string>& headers);

    /**
     * @brief 设置 Content-Type
     * @param contentType 内容类型
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& contentType(const std::string& contentType);

    /**
     * @brief 设置响应体
     * @param body 响应体内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& body(const std::string& body);

    /**
     * @brief 设置响应体（移动语义）
     * @param body 响应体内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& body(std::string&& body);

    /**
     * @brief 设置 JSON 响应体（自动设置 Content-Type）
     * @param json JSON 字符串
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& json(const std::string& json);

    /**
     * @brief 设置 HTML 响应体（自动设置 Content-Type）
     * @param html HTML 字符串
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& html(const std::string& html);

    /**
     * @brief 设置纯文本响应体（自动设置 Content-Type）
     * @param text 文本内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& text(const std::string& text);

    /**
     * @brief 构建 HttpResponse 对象
     * @return HttpResponse 对象
     */
    HttpResponse build();

    /**
     * @brief 构建 HttpResponse 对象（移动语义）
     * @return HttpResponse 对象
     */
    HttpResponse buildMove();

    /**
     * @brief 创建 200 OK 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder ok();

    /**
     * @brief 创建 201 Created 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder created();

    /**
     * @brief 创建 204 No Content 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder noContent();

    /**
     * @brief 创建 400 Bad Request 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder badRequest();

    /**
     * @brief 创建 401 Unauthorized 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder unauthorized();

    /**
     * @brief 创建 403 Forbidden 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder forbidden();

    /**
     * @brief 创建 404 Not Found 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder notFound();

    /**
     * @brief 创建 500 Internal Server Error 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder internalServerError();

private:
    HttpResponse m_response;
    std::string m_body;
};

} // namespace galay::http

#endif // GALAY_HTTP_BUILDER_H
