#include "http_helper.h"
#include "../builder/http_builder.h"
#include "../../galay-utils/crypto/sha1.hpp"
#include "../../galay-utils/encoding/base64.hpp"

// WebSocket 相关功能已经拆到独立 ws 模块。
// TODO: WebSocket 将在单独的仓库实现
#ifdef ENABLE_WEBSOCKET
#include "../../galay-ws/protoc/ws_base.h"
#endif

namespace galay::http 
{
namespace {

HttpResponse build_html_response(HttpStatusCode code, std::string body)
{
    return Http1_1ResponseBuilder()
        .status(code)
        .header("Server", GALAY_SERVER)
        .header("Content-Type", "text/html")
        .body(std::move(body))
        .build_move();
}

} // namespace

    HttpRequest HttpHelper::default_get(std::string_view uri)
    {
        return Http1_1RequestBuilder::get(std::string(uri))
            .user_agent(SERVER_NAME)
            .header("Accept", "*/*")
            .build_move();
    }

    HttpRequest HttpHelper::default_post(std::string_view uri, std::string&& body)
    {
        auto builder = Http1_1RequestBuilder::post(std::string(uri));
        builder.user_agent(SERVER_NAME).header("Accept", "*/*");
        if (!body.empty()) {
            builder.content_type("application/x-www-form-urlencoded").body(std::move(body));
        }
        return builder.build_move();
    }

    HttpRequest HttpHelper::default_put(std::string_view uri, std::string&& body)
    {
        auto builder = Http1_1RequestBuilder::put(std::string(uri));
        builder.user_agent(SERVER_NAME).header("Accept", "*/*");
        if (!body.empty()) {
            builder.content_type("application/x-www-form-urlencoded").body(std::move(body));
        }
        return builder.build_move();
    }

    HttpRequest HttpHelper::default_delete(std::string_view uri)
    {
        return Http1_1RequestBuilder::del(std::string(uri))
            .user_agent(SERVER_NAME)
            .header("Accept", "*/*")
            .build_move();
    }

    HttpRequest HttpHelper::default_patch(std::string_view uri, std::string&& body)
    {
        auto builder = Http1_1RequestBuilder::patch(std::string(uri));
        builder.user_agent(SERVER_NAME).header("Accept", "*/*");
        if (!body.empty()) {
            builder.content_type("application/x-www-form-urlencoded").body(std::move(body));
        }
        return builder.build_move();
    }

    HttpRequest HttpHelper::default_head(std::string_view uri)
    {
        return Http1_1RequestBuilder::head(std::string(uri))
            .user_agent(SERVER_NAME)
            .header("Accept", "*/*")
            .build_move();
    }

    HttpRequest HttpHelper::default_options(std::string_view uri)
    {
        return Http1_1RequestBuilder::options(std::string(uri))
            .user_agent(SERVER_NAME)
            .header("Accept", "*/*")
            .build_move();
    }

    HttpResponse HttpHelper::default_bad_request()
    {
        return build_html_response(HttpStatusCode::BadRequest_400, "<html><body><h1>400 Bad Request</h1></body></html>");
    }

    
    HttpResponse HttpHelper::default_internal_server_error()
    {
        return build_html_response(HttpStatusCode::InternalServerError_500, "<html><body><h1>500 Internal Server Error</h1></body></html>");
    }

    HttpResponse HttpHelper::default_not_found()
    {
        return build_html_response(HttpStatusCode::NotFound_404, "<html><body><h1>404 Not Found</h1></body></html>");
    }

    HttpResponse HttpHelper::default_method_not_allowed()
    {
        return build_html_response(HttpStatusCode::MethodNotAllowed_405, "<html><body><h1>405 Method Not Allowed</h1></body></html>");
    }

    HttpResponse HttpHelper::default_request_timeout()
    {
        return build_html_response(HttpStatusCode::RequestTimeout_408, "<html><body><h1>408 Request Timeout</h1></body></html>");
    }

    HttpResponse HttpHelper::default_too_many_requests()
    {
        return build_html_response(HttpStatusCode::TooManyRequests_429, "<html><body><h1>429 Too Many Requests</h1></body></html>");
    }

    HttpResponse HttpHelper::default_not_implemented()
    {
        return build_html_response(HttpStatusCode::NotImplemented_501, "<html><body><h1>501 Not Implemented</h1></body></html>");
    }

    HttpResponse HttpHelper::default_service_unavailable()
    {
        return build_html_response(HttpStatusCode::ServiceUnavailable_503, "<html><body><h1>503 Service Unavailable</h1></body></html>");
    }

    HttpResponse HttpHelper::default_continue()
    {
        return build_html_response(HttpStatusCode::Continue_100, "<html><body><h1>100 Continue</h1></body></html>");
    }

    HttpResponse HttpHelper::default_switching_protocol()
    {
        return build_html_response(HttpStatusCode::SwitchingProtocol_101, "<html><body><h1>101 Switching Protocol</h1></body></html>");
    }

    HttpResponse HttpHelper::default_processing()
    {
        return build_html_response(HttpStatusCode::Processing_102, "<html><body><h1>102 Processing</h1></body></html>");
    }

    HttpResponse HttpHelper::default_early_hints()
    {
        return build_html_response(HttpStatusCode::EarlyHints_103, "<html><body><h1>103 Early Hints</h1></body></html>");
    }

    HttpResponse HttpHelper::default_created()
    {
        return build_html_response(HttpStatusCode::Created_201, "<html><body><h1>201 Created</h1></body></html>");
    }

    HttpResponse HttpHelper::default_accepted()
    {
        return build_html_response(HttpStatusCode::Accepted_202, "<html><body><h1>202 Accepted</h1></body></html>");
    }

    HttpResponse HttpHelper::default_non_authoritative_information()
    {
        return build_html_response(HttpStatusCode::NonAuthoritativeInformation_203, "<html><body><h1>203 Non-Authoritative Information</h1></body></html>");
    }

    HttpResponse HttpHelper::default_no_content()
    {
        return build_html_response(HttpStatusCode::NoContent_204, "<html><body><h1>204 No Content</h1></body></html>");
    }

    HttpResponse HttpHelper::default_reset_content()
    {
        return build_html_response(HttpStatusCode::ResetContent_205, "<html><body><h1>205 Reset Content</h1></body></html>");
    }

    HttpResponse HttpHelper::default_partial_content()
    {
        return build_html_response(HttpStatusCode::PartialContent_206, "<html><body><h1>206 Partial Content</h1></body></html>");
    }

    HttpResponse HttpHelper::default_multi_status()
    {
        return build_html_response(HttpStatusCode::MultiStatus_207, "<html><body><h1>207 Multi-Status</h1></body></html>");
    }

    HttpResponse HttpHelper::default_already_reported()
    {
        return build_html_response(HttpStatusCode::AlreadyReported_208, "<html><body><h1>208 Already Reported</h1></body></html>");
    }

    HttpResponse HttpHelper::default_im_used()
    {
        return build_html_response(HttpStatusCode::IMUsed_226, "<html><body><h1>226 IM Used</h1></body></html>");
    }

    HttpResponse HttpHelper::default_multiple_choices()
    {
        return build_html_response(HttpStatusCode::MultipleChoices_300, "<html><body><h1>300 Multiple Choices</h1></body></html>");
    }

    HttpResponse HttpHelper::default_moved_permanently()
    {
        return build_html_response(HttpStatusCode::MovedPermanently_301, "<html><body><h1>301 Moved Permanently</h1></body></html>");
    }

    HttpResponse HttpHelper::default_found()
    {
        return build_html_response(HttpStatusCode::Found_302, "<html><body><h1>302 Found</h1></body></html>");
    }

    HttpResponse HttpHelper::default_see_other()
    {
        return build_html_response(HttpStatusCode::SeeOther_303, "<html><body><h1>303 See Other</h1></body></html>");
    }

    HttpResponse HttpHelper::default_not_modified()
    {
        return build_html_response(HttpStatusCode::NotModified_304, "<html><body><h1>304 Not Modified</h1></body></html>");
    }

    HttpResponse HttpHelper::default_use_proxy()
    {
        return build_html_response(HttpStatusCode::UseProxy_305, "<html><body><h1>305 Use Proxy</h1></body></html>");
    }

    HttpResponse HttpHelper::default_unused()
    {
        return build_html_response(HttpStatusCode::Unused_306, "<html><body><h1>306 unused</h1></body></html>");
    }

    HttpResponse HttpHelper::default_temporary_redirect()
    {
        return build_html_response(HttpStatusCode::TemporaryRedirect_307, "<html><body><h1>307 Temporary Redirect</h1></body></html>");
    }

    HttpResponse HttpHelper::default_permanent_redirect()
    {
        return build_html_response(HttpStatusCode::PermanentRedirect_308, "<html><body><h1>308 Permanent Redirect</h1></body></html>");
    }

    HttpResponse HttpHelper::default_unauthorized()
    {
        return build_html_response(HttpStatusCode::Unauthorized_401, "<html><body><h1>401 Unauthorized</h1></body></html>");
    }

    HttpResponse HttpHelper::default_payment_required()
    {
        return build_html_response(HttpStatusCode::PaymentRequired_402, "<html><body><h1>402 Payment Required</h1></body></html>");
    }

    HttpResponse HttpHelper::default_forbidden()
    {
        return build_html_response(HttpStatusCode::Forbidden_403, "<html><body><h1>403 Forbidden</h1></body></html>");
    }

    HttpResponse HttpHelper::default_conflict()
    {
        return build_html_response(HttpStatusCode::Conflict_409, "<html><body><h1>409 Conflict</h1></body></html>");
    }

    HttpResponse HttpHelper::default_not_acceptable()
    {
        return build_html_response(HttpStatusCode::NotAcceptable_406, "<html><body><h1>406 Not Acceptable</h1></body></html>");
    }

    HttpResponse HttpHelper::default_proxy_authentication_required()
    {
        return build_html_response(HttpStatusCode::ProxyAuthenticationRequired_407, "<html><body><h1>407 Proxy Authentication Required</h1></body></html>");
    }

    HttpResponse HttpHelper::default_gone()
    {
        return build_html_response(HttpStatusCode::Gone_410, "<html><body><h1>410 Gone</h1></body></html>");
    }

    HttpResponse HttpHelper::default_length_required()
    {
        return build_html_response(HttpStatusCode::LengthRequired_411, "<html><body><h1>411 Length Required</h1></body></html>");
    }

    HttpResponse HttpHelper::default_precondition_failed()
    {
        return build_html_response(HttpStatusCode::PreconditionFailed_412, "<html><body><h1>412 Precondition Failed</h1></body></html>");
    }

    HttpResponse HttpHelper::default_payload_too_large()
    {
        return build_html_response(HttpStatusCode::PayloadTooLarge_413, "<html><body><h1>413 Payload Too Large</h1></body></html>");
    }

    HttpResponse HttpHelper::default_uri_too_long()
    {
        return build_html_response(HttpStatusCode::UriTooLong_414, "<html><body><h1>414 URI Too Long</h1></body></html>");
    }

    HttpResponse HttpHelper::default_unsupported_media_type()
    {
        return build_html_response(HttpStatusCode::UnsupportedMediaType_415, "<html><body><h1>415 Unsupported Media Type</h1></body></html>");
    }

    HttpResponse HttpHelper::default_range_not_satisfiable()
    {
        return build_html_response(HttpStatusCode::RangeNotSatisfiable_416, "<html><body><h1>416 Range Not Satisfiable</h1></body></html>");
    }

    HttpResponse HttpHelper::default_expectation_failed()
    {
        return build_html_response(HttpStatusCode::ExpectationFailed_417, "<html><body><h1>417 Expectation Failed</h1></body></html>");
    }

    HttpResponse HttpHelper::default_im_a_teapot()
    {
        return build_html_response(HttpStatusCode::ImATeapot_418, "<html><body><h1>418 I'm a teapot</h1></body></html>");
    }

    HttpResponse HttpHelper::default_misdirected_request()
    {
        return build_html_response(HttpStatusCode::MisdirectedRequest_421, "<html><body><h1>421 Misdirected Request</h1></body></html>");
    }

    HttpResponse HttpHelper::default_unprocessable_content()
    {
        return build_html_response(HttpStatusCode::UnprocessableContent_422, "<html><body><h1>422 Unprocessable Content</h1></body></html>");
    }

    HttpResponse HttpHelper::default_locked()
    {
        return build_html_response(HttpStatusCode::Locked_423, "<html><body><h1>423 Locked</h1></body></html>");
    }

    HttpResponse HttpHelper::default_failed_dependency()
    {
        return build_html_response(HttpStatusCode::FailedDependency_424, "<html><body><h1>424 Failed Dependency</h1></body></html>");
    }

    HttpResponse HttpHelper::default_too_early()
    {
        return build_html_response(HttpStatusCode::TooEarly_425, "<html><body><h1>425 Too Early</h1></body></html>");
    }

    HttpResponse HttpHelper::default_upgrade_required()
    {
        return build_html_response(HttpStatusCode::UpgradeRequired_426, "<html><body><h1>426 Upgrade Required</h1></body></html>");
    }

    HttpResponse HttpHelper::default_precondition_required()
    {
        return build_html_response(HttpStatusCode::PreconditionRequired_428, "<html><body><h1>428 Precondition Required</h1></body></html>");
    }

    HttpResponse HttpHelper::default_request_header_fields_too_large()
    {
        return build_html_response(HttpStatusCode::RequestHeaderFieldsTooLarge_431, "<html><body><h1>431 Request Header Fields Too Large</h1></body></html>");
    }

    HttpResponse HttpHelper::default_unavailable_for_legal_reasons()
    {
        return build_html_response(HttpStatusCode::UnavailableForLegalReasons_451, "<html><body><h1>451 Unavailable For Legal Reasons</h1></body></html>");
    }

    HttpResponse HttpHelper::default_bad_gateway()
    {
        return build_html_response(HttpStatusCode::BadGateway_502, "<html><body><h1>502 Bad Gateway</h1></body></html>");
    }

    HttpResponse HttpHelper::default_gateway_timeout()
    {
        return build_html_response(HttpStatusCode::GatewayTimeout_504, "<html><body><h1>504 Gateway Timeout</h1></body></html>");
    }

    HttpResponse HttpHelper::default_http_version_not_supported()
    {
        return build_html_response(HttpStatusCode::HttpVersionNotSupported_505, "<html><body><h1>505 HTTP Version Not Supported</h1></body></html>");
    }

    HttpResponse HttpHelper::default_variant_also_negotiates()
    {
        return build_html_response(HttpStatusCode::VariantAlsoNegotiates_506, "<html><body><h1>506 Variant Also Negotiates</h1></body></html>");
    }

    HttpResponse HttpHelper::default_insufficient_storage()
    {
        return build_html_response(HttpStatusCode::InsufficientStorage_507, "<html><body><h1>507 Insufficient Storage</h1></body></html>");
    }

    HttpResponse HttpHelper::default_loop_detected()
    {
        return build_html_response(HttpStatusCode::LoopDetected_508, "<html><body><h1>508 Loop Detected</h1></body></html>");
    }

    HttpResponse HttpHelper::default_not_extended()
    {
        return build_html_response(HttpStatusCode::NotExtended_510, "<html><body><h1>510 Not Extended</h1></body></html>");
    }

    HttpResponse HttpHelper::default_network_authentication_required()
    {
        return build_html_response(HttpStatusCode::NetworkAuthenticationRequired_511, "<html><body><h1>511 Network Authentication Required</h1></body></html>");
    }

    // 成功响应
    HttpResponse HttpHelper::default_ok(const std::string& type, std::string&& body)
    {
        Http1_1ResponseBuilder builder;
        builder
            .status(HttpStatusCode::OK_200)
            .header("Server", GALAY_SERVER)
            .header("Content-Type", MimeType::convert_to_mime_type(type));
        if (!body.empty()) {
            builder.body(std::move(body));
        }
        return builder.build_move();
    }


    HttpResponse HttpHelper::default_http_response(HttpStatusCode code)
    {
        switch (code)
        {
            case HttpStatusCode::Continue_100:
                return default_continue();
            case HttpStatusCode::SwitchingProtocol_101:
                return default_switching_protocol();
            case HttpStatusCode::Processing_102:
                return default_processing();
            case HttpStatusCode::EarlyHints_103:
                return default_early_hints();
            case HttpStatusCode::OK_200:
                return default_ok("html", "<html><body><h1>200 OK</h1></body></html>");
            case HttpStatusCode::Created_201:
                return default_created();
            case HttpStatusCode::Accepted_202:
                return default_accepted();
            case HttpStatusCode::NonAuthoritativeInformation_203:
                return default_non_authoritative_information();
            case HttpStatusCode::NoContent_204:
                return default_no_content();
            case HttpStatusCode::ResetContent_205:
                return default_reset_content();
            case HttpStatusCode::PartialContent_206:
                return default_partial_content();
            case HttpStatusCode::MultiStatus_207:
                return default_multi_status();
            case HttpStatusCode::AlreadyReported_208:
                return default_already_reported();
            case HttpStatusCode::IMUsed_226:
                return default_im_used();
            case HttpStatusCode::MultipleChoices_300:
                return default_multiple_choices();
            case HttpStatusCode::MovedPermanently_301:
                return default_moved_permanently();
            case HttpStatusCode::Found_302:
                return default_found();
            case HttpStatusCode::SeeOther_303:
                return default_see_other();
            case HttpStatusCode::NotModified_304:
                return default_not_modified();
            case HttpStatusCode::UseProxy_305:
                return default_use_proxy();
            case HttpStatusCode::Unused_306:
                return default_unused();
            case HttpStatusCode::TemporaryRedirect_307:
                return default_temporary_redirect();
            case HttpStatusCode::PermanentRedirect_308:
                return default_permanent_redirect();
            case HttpStatusCode::BadRequest_400:
                return default_bad_request();
            case HttpStatusCode::Unauthorized_401:
                return default_unauthorized();
            case HttpStatusCode::PaymentRequired_402:
                return default_payment_required();
            case HttpStatusCode::Forbidden_403:
                return default_forbidden();
            case HttpStatusCode::NotFound_404:
                return default_not_found();
            case HttpStatusCode::MethodNotAllowed_405:
                return default_method_not_allowed();
            case HttpStatusCode::NotAcceptable_406:
                return default_not_acceptable();
            case HttpStatusCode::ProxyAuthenticationRequired_407:
                return default_proxy_authentication_required();
            case HttpStatusCode::RequestTimeout_408:
                return default_request_timeout();
            case HttpStatusCode::Conflict_409:
                return default_conflict();
            case HttpStatusCode::Gone_410:
                return default_gone();
            case HttpStatusCode::LengthRequired_411:
                return default_length_required();
            case HttpStatusCode::PreconditionFailed_412:
                return default_precondition_failed();
            case HttpStatusCode::PayloadTooLarge_413:
                return default_payload_too_large();
            case HttpStatusCode::UriTooLong_414:
                return default_uri_too_long();
            case HttpStatusCode::UnsupportedMediaType_415:
                return default_unsupported_media_type();
            case HttpStatusCode::RangeNotSatisfiable_416:
                return default_range_not_satisfiable();
            case HttpStatusCode::ExpectationFailed_417:
                return default_expectation_failed();
            case HttpStatusCode::ImATeapot_418:
                return default_im_a_teapot();
            case HttpStatusCode::MisdirectedRequest_421:
                return default_misdirected_request();
            case HttpStatusCode::UnprocessableContent_422:
                return default_unprocessable_content();
            case HttpStatusCode::Locked_423:
                return default_locked();
            case HttpStatusCode::FailedDependency_424:
                return default_failed_dependency();
            case HttpStatusCode::TooEarly_425:
                return default_too_early();
            case HttpStatusCode::UpgradeRequired_426:
                return default_upgrade_required();
            case HttpStatusCode::PreconditionRequired_428:
                return default_precondition_required();
            case HttpStatusCode::TooManyRequests_429:
                return default_too_many_requests();
            case HttpStatusCode::RequestHeaderFieldsTooLarge_431:
                return default_request_header_fields_too_large();
            case HttpStatusCode::UnavailableForLegalReasons_451:
                return default_unavailable_for_legal_reasons();
            case HttpStatusCode::InternalServerError_500:
                return default_internal_server_error();
            case HttpStatusCode::NotImplemented_501:
                return default_not_implemented();
            case HttpStatusCode::BadGateway_502:
                return default_bad_gateway();
            case HttpStatusCode::ServiceUnavailable_503:
                return default_service_unavailable();
            case HttpStatusCode::GatewayTimeout_504:
                return default_gateway_timeout();
            case HttpStatusCode::HttpVersionNotSupported_505:
                return default_http_version_not_supported();
            case HttpStatusCode::VariantAlsoNegotiates_506:
                return default_variant_also_negotiates();
            case HttpStatusCode::InsufficientStorage_507:
                return default_insufficient_storage();
            case HttpStatusCode::LoopDetected_508:
                return default_loop_detected();
            case HttpStatusCode::NotExtended_510:
                return default_not_extended();
            case HttpStatusCode::NetworkAuthenticationRequired_511:
                return default_network_authentication_required();
            default:
                return default_internal_server_error();
        }
    }

#ifdef ENABLE_WEBSOCKET
    // WebSocket 相关实现
    std::string HttpHelper::generate_web_socket_accept_key(const std::string& clientKey)
    {
        const std::string combined = clientKey + WS_MAGIC_STRING;
        const auto digest = galay::utils::SHA1::hash(
            reinterpret_cast<const uint8_t*>(combined.data()), combined.size());
        return galay::utils::Base64Util::base64_encode(
            reinterpret_cast<const unsigned char*>(digest.data()), digest.size());
    }

    HttpResponse HttpHelper::create_web_socket_upgrade_response(const std::string& clientKey)
    {
        return Http1_1ResponseBuilder()
            .status(HttpStatusCode::SwitchingProtocol_101)
            .header("Upgrade", "websocket")
            .header("Connection", "Upgrade")
            .header("Sec-WebSocket-Accept", generate_web_socket_accept_key(clientKey))
            .header("Server", GALAY_SERVER)
            .build_move();
    }
#else
    // WebSocket 功能禁用时的占位实现
    std::string HttpHelper::generate_web_socket_accept_key(const std::string& clientKey)
    {
        (void)clientKey;
        return "";
    }

    HttpResponse HttpHelper::create_web_socket_upgrade_response(const std::string& clientKey)
    {
        (void)clientKey;
        return default_not_implemented();
    }
#endif
}
