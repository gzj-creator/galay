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

http::HttpResponseResult make_response(int status, std::string body, bool json_body, bool head) {
    http::HttpResponse response;
    response.header().version() = http::HttpVersion::HttpVersion_1_1;
    response.header().code() = static_cast<http::HttpStatusCode>(status);
    auto& headers = response.header().header_pairs();
    if (json_body && !head && status != 204 && status != 205) {
        const auto content_type = headers.add_header_pair("Content-Type", "application/json; charset=utf-8");
        if (content_type != http::kNoError) {
            return std::unexpected(http::HttpError{content_type, "cannot create Content-Type header"});
        }
        response.set_body_str(std::move(body));
    }
    return response;
}

} // namespace router_detail

ApiResult<PreparedApi> ApiBuilder::build() {
    if (built_) return std::unexpected(ApiError{ApiErrorCode::kFrozenBuilder, "API builder has already been built", 500});
    auto document = render_openapi(info_, endpoints_);
    if (!document) return std::unexpected(std::move(document.error()));
    http::HttpRouter router;
    for (const auto& route : routes_) {
        switch (route.method) {
        case http::HttpMethod::GET: router.add_request_handler<http::HttpMethod::GET>(route.path, route.handler); break;
        case http::HttpMethod::POST: router.add_request_handler<http::HttpMethod::POST>(route.path, route.handler); break;
        case http::HttpMethod::HEAD: router.add_request_handler<http::HttpMethod::HEAD>(route.path, route.handler); break;
        case http::HttpMethod::PUT: router.add_request_handler<http::HttpMethod::PUT>(route.path, route.handler); break;
        case http::HttpMethod::DELETE: router.add_request_handler<http::HttpMethod::DELETE>(route.path, route.handler); break;
        case http::HttpMethod::PATCH: router.add_request_handler<http::HttpMethod::PATCH>(route.path, route.handler); break;
        case http::HttpMethod::OPTIONS: router.add_request_handler<http::HttpMethod::OPTIONS>(route.path, route.handler); break;
        case http::HttpMethod::TRACE: router.add_request_handler<http::HttpMethod::TRACE>(route.path, route.handler); break;
        default: return std::unexpected(ApiError{ApiErrorCode::kInvalidMetadata, "unsupported route method", 500});
        }
    }
    auto shared_document = std::make_shared<const std::string>(std::move(*document));
    built_ = true;
    routes_.clear();
    return PreparedApi{std::move(router), std::move(shared_document), std::move(endpoints_)};
}

} // namespace galay::api
