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

} // namespace galay::api
