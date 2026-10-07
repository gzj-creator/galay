/**
 * @file http_builder.cc
 * @brief HTTP/1.1 request and response builder implementations.
 */

#include "http_builder.h"

#include <sstream>
#include <utility>

namespace galay::http
{

Http1_1RequestBuilder::Http1_1RequestBuilder(HeaderPair::Mode mode)
{
    m_request.header().version() = HttpVersion::HttpVersion_1_1;
    m_request.header().method() = HttpMethod::GET;
    m_request.header().header_pairs() = HeaderPair(mode);
}

Http1_1RequestBuilder Http1_1RequestBuilder::clone() const
{
    Http1_1RequestBuilder copy;
    copy.m_request = m_request.clone();
    copy.m_body = m_body;
    return copy;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::method(HttpMethod method)
{
    m_request.header().method() = method;
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::uri(const std::string& uri)
{
    m_request.header().uri() = uri;
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::header(const std::string& key, const std::string& value)
{
    m_request.header().header_pairs().add_header_pair(key, value);
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::headers(const std::map<std::string, std::string>& headers)
{
    for (const auto& [key, value] : headers) {
        m_request.header().header_pairs().add_header_pair(key, value);
    }
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::host(const std::string& host)
{
    return header("Host", host);
}

Http1_1RequestBuilder& Http1_1RequestBuilder::content_type(const std::string& content_type)
{
    return header("Content-Type", content_type);
}

Http1_1RequestBuilder& Http1_1RequestBuilder::user_agent(const std::string& user_agent)
{
    return header("User-Agent", user_agent);
}

Http1_1RequestBuilder& Http1_1RequestBuilder::connection(const std::string& connection)
{
    return header("Connection", connection);
}

Http1_1RequestBuilder& Http1_1RequestBuilder::body(const std::string& body)
{
    m_body = body;
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::body(std::string&& body)
{
    m_body = std::move(body);
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::json(const std::string& json)
{
    content_type("application/json; charset=utf-8");
    m_body = json;
    return *this;
}

Http1_1RequestBuilder& Http1_1RequestBuilder::form(const std::map<std::string, std::string>& form)
{
    content_type("application/x-www-form-urlencoded");

    std::ostringstream oss;
    bool first = true;
    for (const auto& [key, value] : form) {
        if (!first) {
            oss << "&";
        }
        oss << key << "=" << value;  // TODO: URL encode
        first = false;
    }

    m_body = oss.str();
    return *this;
}

HttpRequest Http1_1RequestBuilder::build()
{
    HttpRequest request_copy = m_request.clone();

    if (!m_body.empty()) {
        std::string body_copy = m_body;
        request_copy.set_body_str(std::move(body_copy));
    }
    return request_copy;
}

HttpRequest Http1_1RequestBuilder::build_move()
{
    if (!m_body.empty()) {
        m_request.set_body_str(std::move(m_body));
    }
    return std::move(m_request);
}

Http1_1RequestBuilder Http1_1RequestBuilder::get(const std::string& uri, HeaderPair::Mode mode)
{
    Http1_1RequestBuilder builder(mode);
    builder.method(HttpMethod::GET).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::post(const std::string& uri, HeaderPair::Mode mode)
{
    Http1_1RequestBuilder builder(mode);
    builder.method(HttpMethod::POST).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::put(const std::string& uri, HeaderPair::Mode mode)
{
    Http1_1RequestBuilder builder(mode);
    builder.method(HttpMethod::PUT).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::del(const std::string& uri, HeaderPair::Mode mode)
{
    Http1_1RequestBuilder builder(mode);
    builder.method(HttpMethod::DELETE).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::patch(const std::string& uri, HeaderPair::Mode mode)
{
    Http1_1RequestBuilder builder(mode);
    builder.method(HttpMethod::PATCH).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::head(const std::string& uri, HeaderPair::Mode mode)
{
    Http1_1RequestBuilder builder(mode);
    builder.method(HttpMethod::HEAD).uri(uri);
    return builder;
}

Http1_1RequestBuilder Http1_1RequestBuilder::options(const std::string& uri, HeaderPair::Mode mode)
{
    Http1_1RequestBuilder builder(mode);
    builder.method(HttpMethod::OPTIONS).uri(uri);
    return builder;
}

Http1_1ResponseBuilder::Http1_1ResponseBuilder()
{
    m_response.header().version() = HttpVersion::HttpVersion_1_1;
    m_response.header().code() = HttpStatusCode::OK_200;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::clone() const
{
    Http1_1ResponseBuilder copy;
    copy.m_response = m_response.clone();
    copy.m_body = m_body;
    return copy;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::status(int code)
{
    m_response.header().code() = static_cast<HttpStatusCode>(code);
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::status(HttpStatusCode code)
{
    m_response.header().code() = code;
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::header(const std::string& key, const std::string& value)
{
    m_response.header().header_pairs().add_header_pair(key, value);
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::headers(const std::map<std::string, std::string>& headers)
{
    for (const auto& [key, value] : headers) {
        m_response.header().header_pairs().add_header_pair(key, value);
    }
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::content_type(const std::string& content_type)
{
    return header("Content-Type", content_type);
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::body(const std::string& body)
{
    m_body = body;
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::body(std::string&& body)
{
    m_body = std::move(body);
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::json(const std::string& json)
{
    content_type("application/json; charset=utf-8");
    m_body = json;
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::html(const std::string& html)
{
    content_type("text/html; charset=utf-8");
    m_body = html;
    return *this;
}

Http1_1ResponseBuilder& Http1_1ResponseBuilder::text(const std::string& text)
{
    content_type("text/plain; charset=utf-8");
    m_body = text;
    return *this;
}

HttpResponse Http1_1ResponseBuilder::build()
{
    HttpResponse response_copy = m_response.clone();

    if (!m_body.empty()) {
        std::string body_copy = m_body;
        response_copy.set_body_str(std::move(body_copy));
    }
    return response_copy;
}

HttpResponse Http1_1ResponseBuilder::build_move()
{
    if (!m_body.empty()) {
        m_response.set_body_str(std::move(m_body));
    }
    return std::move(m_response);
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::ok()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::OK_200);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::created()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::Created_201);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::no_content()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::NoContent_204);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::bad_request()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::BadRequest_400);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::unauthorized()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::Unauthorized_401);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::forbidden()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::Forbidden_403);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::not_found()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::NotFound_404);
    return builder;
}

Http1_1ResponseBuilder Http1_1ResponseBuilder::internal_server_error()
{
    Http1_1ResponseBuilder builder;
    builder.status(HttpStatusCode::InternalServerError_500);
    return builder;
}

} // namespace galay::http
