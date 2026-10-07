#include "api_router.h"

#include <map>

namespace galay::api {
namespace router_detail {

ApiResult<std::string> encode_error(const ApiError& error) {
    auto encoded = json::serialize(std::map<std::string, std::string>{
        {"code", std::string(api_error_name(error.code))}, {"message", error.message}});
    if (!encoded) return std::unexpected(ApiError{ApiErrorCode::kEncodingError, std::move(encoded.error()), 500});
    return std::move(*encoded);
}

kernel::Task<ApiResult<void>> send_response(http::HttpConn& connection, int status,
    std::string body, bool json_body, bool head, bool keep_alive) {
    http::HttpResponse response;
    response.header().version() = http::HttpVersion::HttpVersion_1_1;
    response.header().code() = static_cast<http::HttpStatusCode>(status);
    auto& headers = response.header().header_pairs();
    const auto connection_header = headers.add_header_pair("Connection", keep_alive ? "keep-alive" : "close");
    if (connection_header != http::kNoError) {
        co_return std::unexpected(ApiError{ApiErrorCode::kEncodingError,
            "cannot create Connection header: " + std::to_string(static_cast<int>(connection_header)), 500});
    }
    if (json_body && !head && status != 204 && status != 205) {
        const auto content_type = headers.add_header_pair("Content-Type", "application/json; charset=utf-8");
        if (content_type != http::kNoError) {
            co_return std::unexpected(ApiError{ApiErrorCode::kEncodingError,
                "cannot create Content-Type header: " + std::to_string(static_cast<int>(content_type)), 500});
        }
        response.set_body_str(std::move(body));
    }
    auto writer = connection.get_writer();
    if (head || status == 204) {
        // send_response inserts Content-Length; HTTP forbids that field on a 204.
        const auto sent = co_await writer.send(response.header().to_string());
        if (!sent || !*sent) {
            co_return std::unexpected(ApiError{ApiErrorCode::kTransportError,
                sent ? "HTTP writer returned false" : sent.error().message(), 500});
        }
    } else {
        const auto sent = co_await writer.send_response(std::move(response));
        if (!sent || !*sent) {
            co_return std::unexpected(ApiError{ApiErrorCode::kTransportError,
                sent ? "HTTP writer returned false" : sent.error().message(), 500});
        }
    }
    co_return ApiResult<void>{};
}

} // namespace router_detail

ApiResult<PreparedApi> ApiBuilder::build() {
    if (built_) return std::unexpected(ApiError{ApiErrorCode::kFrozenBuilder, "API builder has already been built", 500});
    auto document = render_openapi(info_, endpoints_);
    if (!document) return std::unexpected(std::move(document.error()));
    http::HttpRouter router;
    for (const auto& route : routes_) {
        switch (route.method) {
        case http::HttpMethod::GET: router.add_handler<http::HttpMethod::GET>(route.path, route.handler); break;
        case http::HttpMethod::POST: router.add_handler<http::HttpMethod::POST>(route.path, route.handler); break;
        case http::HttpMethod::HEAD: router.add_handler<http::HttpMethod::HEAD>(route.path, route.handler); break;
        case http::HttpMethod::PUT: router.add_handler<http::HttpMethod::PUT>(route.path, route.handler); break;
        case http::HttpMethod::DELETE: router.add_handler<http::HttpMethod::DELETE>(route.path, route.handler); break;
        case http::HttpMethod::PATCH: router.add_handler<http::HttpMethod::PATCH>(route.path, route.handler); break;
        case http::HttpMethod::OPTIONS: router.add_handler<http::HttpMethod::OPTIONS>(route.path, route.handler); break;
        case http::HttpMethod::TRACE: router.add_handler<http::HttpMethod::TRACE>(route.path, route.handler); break;
        default: return std::unexpected(ApiError{ApiErrorCode::kInvalidMetadata, "unsupported route method", 500});
        }
    }
    auto shared_document = std::make_shared<const std::string>(std::move(*document));
    built_ = true;
    routes_.clear();
    return PreparedApi{std::move(router), std::move(shared_document), std::move(endpoints_)};
}

} // namespace galay::api
