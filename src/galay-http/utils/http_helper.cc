#include "http_helper.h"
#include "galay-http/builder/http_builder.h"
#include "galay-utils/crypto/sha1.hpp"
#include "galay-utils/encoding/base64.hpp"

// WebSocket 相关功能已经拆到独立 ws 模块。
// TODO: WebSocket 将在单独的仓库实现
#ifdef ENABLE_WEBSOCKET
#include "galay-ws/protoc/ws_base.h"
#endif

namespace galay::http 
{
namespace {

HttpResponse buildHtmlResponse(HttpStatusCode code, std::string body)
{
    return Http1_1ResponseBuilder()
        .status(code)
        .header("Server", GALAY_SERVER)
        .header("Content-Type", "text/html")
        .body(std::move(body))
        .buildMove();
}

} // namespace

    HttpRequest HttpHelper::defaultGet(std::string_view uri)
    {
        return Http1_1RequestBuilder::get(std::string(uri))
            .userAgent(SERVER_NAME)
            .header("Accept", "*/*")
            .buildMove();
    }

    HttpRequest HttpHelper::defaultPost(std::string_view uri, std::string&& body)
    {
        auto builder = Http1_1RequestBuilder::post(std::string(uri));
        builder.userAgent(SERVER_NAME).header("Accept", "*/*");
        if (!body.empty()) {
            builder.contentType("application/x-www-form-urlencoded").body(std::move(body));
        }
        return builder.buildMove();
    }

    HttpRequest HttpHelper::defaultPut(std::string_view uri, std::string&& body)
    {
        auto builder = Http1_1RequestBuilder::put(std::string(uri));
        builder.userAgent(SERVER_NAME).header("Accept", "*/*");
        if (!body.empty()) {
            builder.contentType("application/x-www-form-urlencoded").body(std::move(body));
        }
        return builder.buildMove();
    }

    HttpRequest HttpHelper::defaultDelete(std::string_view uri)
    {
        return Http1_1RequestBuilder::del(std::string(uri))
            .userAgent(SERVER_NAME)
            .header("Accept", "*/*")
            .buildMove();
    }

    HttpRequest HttpHelper::defaultPatch(std::string_view uri, std::string&& body)
    {
        auto builder = Http1_1RequestBuilder::patch(std::string(uri));
        builder.userAgent(SERVER_NAME).header("Accept", "*/*");
        if (!body.empty()) {
            builder.contentType("application/x-www-form-urlencoded").body(std::move(body));
        }
        return builder.buildMove();
    }

    HttpRequest HttpHelper::defaultHead(std::string_view uri)
    {
        return Http1_1RequestBuilder::head(std::string(uri))
            .userAgent(SERVER_NAME)
            .header("Accept", "*/*")
            .buildMove();
    }

    HttpRequest HttpHelper::defaultOptions(std::string_view uri)
    {
        return Http1_1RequestBuilder::options(std::string(uri))
            .userAgent(SERVER_NAME)
            .header("Accept", "*/*")
            .buildMove();
    }

    HttpResponse HttpHelper::defaultBadRequest()
    {
        return buildHtmlResponse(HttpStatusCode::BadRequest_400, "<html><body><h1>400 Bad Request</h1></body></html>");
    }

    
    HttpResponse HttpHelper::defaultInternalServerError()
    {
        return buildHtmlResponse(HttpStatusCode::InternalServerError_500, "<html><body><h1>500 Internal Server Error</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultNotFound()
    {
        return buildHtmlResponse(HttpStatusCode::NotFound_404, "<html><body><h1>404 Not Found</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultMethodNotAllowed()
    {
        return buildHtmlResponse(HttpStatusCode::MethodNotAllowed_405, "<html><body><h1>405 Method Not Allowed</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultRequestTimeout()
    {
        return buildHtmlResponse(HttpStatusCode::RequestTimeout_408, "<html><body><h1>408 Request Timeout</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultTooManyRequests()
    {
        return buildHtmlResponse(HttpStatusCode::TooManyRequests_429, "<html><body><h1>429 Too Many Requests</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultNotImplemented()
    {
        return buildHtmlResponse(HttpStatusCode::NotImplemented_501, "<html><body><h1>501 Not Implemented</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultServiceUnavailable()
    {
        return buildHtmlResponse(HttpStatusCode::ServiceUnavailable_503, "<html><body><h1>503 Service Unavailable</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultContinue()
    {
        return buildHtmlResponse(HttpStatusCode::Continue_100, "<html><body><h1>100 Continue</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultSwitchingProtocol()
    {
        return buildHtmlResponse(HttpStatusCode::SwitchingProtocol_101, "<html><body><h1>101 Switching Protocol</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultProcessing()
    {
        return buildHtmlResponse(HttpStatusCode::Processing_102, "<html><body><h1>102 Processing</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultEarlyHints()
    {
        return buildHtmlResponse(HttpStatusCode::EarlyHints_103, "<html><body><h1>103 Early Hints</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultCreated()
    {
        return buildHtmlResponse(HttpStatusCode::Created_201, "<html><body><h1>201 Created</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultAccepted()
    {
        return buildHtmlResponse(HttpStatusCode::Accepted_202, "<html><body><h1>202 Accepted</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultNonAuthoritativeInformation()
    {
        return buildHtmlResponse(HttpStatusCode::NonAuthoritativeInformation_203, "<html><body><h1>203 Non-Authoritative Information</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultNoContent()
    {
        return buildHtmlResponse(HttpStatusCode::NoContent_204, "<html><body><h1>204 No Content</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultResetContent()
    {
        return buildHtmlResponse(HttpStatusCode::ResetContent_205, "<html><body><h1>205 Reset Content</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultPartialContent()
    {
        return buildHtmlResponse(HttpStatusCode::PartialContent_206, "<html><body><h1>206 Partial Content</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultMultiStatus()
    {
        return buildHtmlResponse(HttpStatusCode::MultiStatus_207, "<html><body><h1>207 Multi-Status</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultAlreadyReported()
    {
        return buildHtmlResponse(HttpStatusCode::AlreadyReported_208, "<html><body><h1>208 Already Reported</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultIMUsed()
    {
        return buildHtmlResponse(HttpStatusCode::IMUsed_226, "<html><body><h1>226 IM Used</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultMultipleChoices()
    {
        return buildHtmlResponse(HttpStatusCode::MultipleChoices_300, "<html><body><h1>300 Multiple Choices</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultMovedPermanently()
    {
        return buildHtmlResponse(HttpStatusCode::MovedPermanently_301, "<html><body><h1>301 Moved Permanently</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultFound()
    {
        return buildHtmlResponse(HttpStatusCode::Found_302, "<html><body><h1>302 Found</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultSeeOther()
    {
        return buildHtmlResponse(HttpStatusCode::SeeOther_303, "<html><body><h1>303 See Other</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultNotModified()
    {
        return buildHtmlResponse(HttpStatusCode::NotModified_304, "<html><body><h1>304 Not Modified</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultUseProxy()
    {
        return buildHtmlResponse(HttpStatusCode::UseProxy_305, "<html><body><h1>305 Use Proxy</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultUnused()
    {
        return buildHtmlResponse(HttpStatusCode::Unused_306, "<html><body><h1>306 unused</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultTemporaryRedirect()
    {
        return buildHtmlResponse(HttpStatusCode::TemporaryRedirect_307, "<html><body><h1>307 Temporary Redirect</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultPermanentRedirect()
    {
        return buildHtmlResponse(HttpStatusCode::PermanentRedirect_308, "<html><body><h1>308 Permanent Redirect</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultUnauthorized()
    {
        return buildHtmlResponse(HttpStatusCode::Unauthorized_401, "<html><body><h1>401 Unauthorized</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultPaymentRequired()
    {
        return buildHtmlResponse(HttpStatusCode::PaymentRequired_402, "<html><body><h1>402 Payment Required</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultForbidden()
    {
        return buildHtmlResponse(HttpStatusCode::Forbidden_403, "<html><body><h1>403 Forbidden</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultConflict()
    {
        return buildHtmlResponse(HttpStatusCode::Conflict_409, "<html><body><h1>409 Conflict</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultNotAcceptable()
    {
        return buildHtmlResponse(HttpStatusCode::NotAcceptable_406, "<html><body><h1>406 Not Acceptable</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultProxyAuthenticationRequired()
    {
        return buildHtmlResponse(HttpStatusCode::ProxyAuthenticationRequired_407, "<html><body><h1>407 Proxy Authentication Required</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultGone()
    {
        return buildHtmlResponse(HttpStatusCode::Gone_410, "<html><body><h1>410 Gone</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultLengthRequired()
    {
        return buildHtmlResponse(HttpStatusCode::LengthRequired_411, "<html><body><h1>411 Length Required</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultPreconditionFailed()
    {
        return buildHtmlResponse(HttpStatusCode::PreconditionFailed_412, "<html><body><h1>412 Precondition Failed</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultPayloadTooLarge()
    {
        return buildHtmlResponse(HttpStatusCode::PayloadTooLarge_413, "<html><body><h1>413 Payload Too Large</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultUriTooLong()
    {
        return buildHtmlResponse(HttpStatusCode::UriTooLong_414, "<html><body><h1>414 URI Too Long</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultUnsupportedMediaType()
    {
        return buildHtmlResponse(HttpStatusCode::UnsupportedMediaType_415, "<html><body><h1>415 Unsupported Media Type</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultRangeNotSatisfiable()
    {
        return buildHtmlResponse(HttpStatusCode::RangeNotSatisfiable_416, "<html><body><h1>416 Range Not Satisfiable</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultExpectationFailed()
    {
        return buildHtmlResponse(HttpStatusCode::ExpectationFailed_417, "<html><body><h1>417 Expectation Failed</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultImATeapot()
    {
        return buildHtmlResponse(HttpStatusCode::ImATeapot_418, "<html><body><h1>418 I'm a teapot</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultMisdirectedRequest()
    {
        return buildHtmlResponse(HttpStatusCode::MisdirectedRequest_421, "<html><body><h1>421 Misdirected Request</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultUnprocessableContent()
    {
        return buildHtmlResponse(HttpStatusCode::UnprocessableContent_422, "<html><body><h1>422 Unprocessable Content</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultLocked()
    {
        return buildHtmlResponse(HttpStatusCode::Locked_423, "<html><body><h1>423 Locked</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultFailedDependency()
    {
        return buildHtmlResponse(HttpStatusCode::FailedDependency_424, "<html><body><h1>424 Failed Dependency</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultTooEarly()
    {
        return buildHtmlResponse(HttpStatusCode::TooEarly_425, "<html><body><h1>425 Too Early</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultUpgradeRequired()
    {
        return buildHtmlResponse(HttpStatusCode::UpgradeRequired_426, "<html><body><h1>426 Upgrade Required</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultPreconditionRequired()
    {
        return buildHtmlResponse(HttpStatusCode::PreconditionRequired_428, "<html><body><h1>428 Precondition Required</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultRequestHeaderFieldsTooLarge()
    {
        return buildHtmlResponse(HttpStatusCode::RequestHeaderFieldsTooLarge_431, "<html><body><h1>431 Request Header Fields Too Large</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultUnavailableForLegalReasons()
    {
        return buildHtmlResponse(HttpStatusCode::UnavailableForLegalReasons_451, "<html><body><h1>451 Unavailable For Legal Reasons</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultBadGateway()
    {
        return buildHtmlResponse(HttpStatusCode::BadGateway_502, "<html><body><h1>502 Bad Gateway</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultGatewayTimeout()
    {
        return buildHtmlResponse(HttpStatusCode::GatewayTimeout_504, "<html><body><h1>504 Gateway Timeout</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultHttpVersionNotSupported()
    {
        return buildHtmlResponse(HttpStatusCode::HttpVersionNotSupported_505, "<html><body><h1>505 HTTP Version Not Supported</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultVariantAlsoNegotiates()
    {
        return buildHtmlResponse(HttpStatusCode::VariantAlsoNegotiates_506, "<html><body><h1>506 Variant Also Negotiates</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultInsufficientStorage()
    {
        return buildHtmlResponse(HttpStatusCode::InsufficientStorage_507, "<html><body><h1>507 Insufficient Storage</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultLoopDetected()
    {
        return buildHtmlResponse(HttpStatusCode::LoopDetected_508, "<html><body><h1>508 Loop Detected</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultNotExtended()
    {
        return buildHtmlResponse(HttpStatusCode::NotExtended_510, "<html><body><h1>510 Not Extended</h1></body></html>");
    }

    HttpResponse HttpHelper::defaultNetworkAuthenticationRequired()
    {
        return buildHtmlResponse(HttpStatusCode::NetworkAuthenticationRequired_511, "<html><body><h1>511 Network Authentication Required</h1></body></html>");
    }

    // 成功响应
    HttpResponse HttpHelper::defaultOk(const std::string& type, std::string&& body)
    {
        auto builder = Http1_1ResponseBuilder()
            .status(HttpStatusCode::OK_200)
            .header("Server", GALAY_SERVER)
            .header("Content-Type", MimeType::convertToMimeType(type));
        if (!body.empty()) {
            builder.body(std::move(body));
        }
        return builder.buildMove();
    }


    HttpResponse HttpHelper::defaultHttpResponse(HttpStatusCode code)
    {
        switch (code)
        {
            case HttpStatusCode::Continue_100:
                return defaultContinue();
            case HttpStatusCode::SwitchingProtocol_101:
                return defaultSwitchingProtocol();
            case HttpStatusCode::Processing_102:
                return defaultProcessing();
            case HttpStatusCode::EarlyHints_103:
                return defaultEarlyHints();
            case HttpStatusCode::OK_200:
                return defaultOk("html", "<html><body><h1>200 OK</h1></body></html>");
            case HttpStatusCode::Created_201:
                return defaultCreated();
            case HttpStatusCode::Accepted_202:
                return defaultAccepted();
            case HttpStatusCode::NonAuthoritativeInformation_203:
                return defaultNonAuthoritativeInformation();
            case HttpStatusCode::NoContent_204:
                return defaultNoContent();
            case HttpStatusCode::ResetContent_205:
                return defaultResetContent();
            case HttpStatusCode::PartialContent_206:
                return defaultPartialContent();
            case HttpStatusCode::MultiStatus_207:
                return defaultMultiStatus();
            case HttpStatusCode::AlreadyReported_208:
                return defaultAlreadyReported();
            case HttpStatusCode::IMUsed_226:
                return defaultIMUsed();
            case HttpStatusCode::MultipleChoices_300:
                return defaultMultipleChoices();
            case HttpStatusCode::MovedPermanently_301:
                return defaultMovedPermanently();
            case HttpStatusCode::Found_302:
                return defaultFound();
            case HttpStatusCode::SeeOther_303:
                return defaultSeeOther();
            case HttpStatusCode::NotModified_304:
                return defaultNotModified();
            case HttpStatusCode::UseProxy_305:
                return defaultUseProxy();
            case HttpStatusCode::Unused_306:
                return defaultUnused();
            case HttpStatusCode::TemporaryRedirect_307:
                return defaultTemporaryRedirect();
            case HttpStatusCode::PermanentRedirect_308:
                return defaultPermanentRedirect();
            case HttpStatusCode::BadRequest_400:
                return defaultBadRequest();
            case HttpStatusCode::Unauthorized_401:
                return defaultUnauthorized();
            case HttpStatusCode::PaymentRequired_402:
                return defaultPaymentRequired();
            case HttpStatusCode::Forbidden_403:
                return defaultForbidden();
            case HttpStatusCode::NotFound_404:
                return defaultNotFound();
            case HttpStatusCode::MethodNotAllowed_405:
                return defaultMethodNotAllowed();
            case HttpStatusCode::NotAcceptable_406:
                return defaultNotAcceptable();
            case HttpStatusCode::ProxyAuthenticationRequired_407:
                return defaultProxyAuthenticationRequired();
            case HttpStatusCode::RequestTimeout_408:
                return defaultRequestTimeout();
            case HttpStatusCode::Conflict_409:
                return defaultConflict();
            case HttpStatusCode::Gone_410:
                return defaultGone();
            case HttpStatusCode::LengthRequired_411:
                return defaultLengthRequired();
            case HttpStatusCode::PreconditionFailed_412:
                return defaultPreconditionFailed();
            case HttpStatusCode::PayloadTooLarge_413:
                return defaultPayloadTooLarge();
            case HttpStatusCode::UriTooLong_414:
                return defaultUriTooLong();
            case HttpStatusCode::UnsupportedMediaType_415:
                return defaultUnsupportedMediaType();
            case HttpStatusCode::RangeNotSatisfiable_416:
                return defaultRangeNotSatisfiable();
            case HttpStatusCode::ExpectationFailed_417:
                return defaultExpectationFailed();
            case HttpStatusCode::ImATeapot_418:
                return defaultImATeapot();
            case HttpStatusCode::MisdirectedRequest_421:
                return defaultMisdirectedRequest();
            case HttpStatusCode::UnprocessableContent_422:
                return defaultUnprocessableContent();
            case HttpStatusCode::Locked_423:
                return defaultLocked();
            case HttpStatusCode::FailedDependency_424:
                return defaultFailedDependency();
            case HttpStatusCode::TooEarly_425:
                return defaultTooEarly();
            case HttpStatusCode::UpgradeRequired_426:
                return defaultUpgradeRequired();
            case HttpStatusCode::PreconditionRequired_428:
                return defaultPreconditionRequired();
            case HttpStatusCode::TooManyRequests_429:
                return defaultTooManyRequests();
            case HttpStatusCode::RequestHeaderFieldsTooLarge_431:
                return defaultRequestHeaderFieldsTooLarge();
            case HttpStatusCode::UnavailableForLegalReasons_451:
                return defaultUnavailableForLegalReasons();
            case HttpStatusCode::InternalServerError_500:
                return defaultInternalServerError();
            case HttpStatusCode::NotImplemented_501:
                return defaultNotImplemented();
            case HttpStatusCode::BadGateway_502:
                return defaultBadGateway();
            case HttpStatusCode::ServiceUnavailable_503:
                return defaultServiceUnavailable();
            case HttpStatusCode::GatewayTimeout_504:
                return defaultGatewayTimeout();
            case HttpStatusCode::HttpVersionNotSupported_505:
                return defaultHttpVersionNotSupported();
            case HttpStatusCode::VariantAlsoNegotiates_506:
                return defaultVariantAlsoNegotiates();
            case HttpStatusCode::InsufficientStorage_507:
                return defaultInsufficientStorage();
            case HttpStatusCode::LoopDetected_508:
                return defaultLoopDetected();
            case HttpStatusCode::NotExtended_510:
                return defaultNotExtended();
            case HttpStatusCode::NetworkAuthenticationRequired_511:
                return defaultNetworkAuthenticationRequired();
            default:
                return defaultInternalServerError();
        }
    }

#ifdef ENABLE_WEBSOCKET
    // WebSocket 相关实现
    std::string HttpHelper::generateWebSocketAcceptKey(const std::string& clientKey)
    {
        const std::string combined = clientKey + WS_MAGIC_STRING;
        const auto digest = galay::utils::SHA1::hash(
            reinterpret_cast<const uint8_t*>(combined.data()), combined.size());
        return galay::utils::Base64Util::Base64Encode(
            reinterpret_cast<const unsigned char*>(digest.data()), digest.size());
    }

    HttpResponse HttpHelper::createWebSocketUpgradeResponse(const std::string& clientKey)
    {
        return Http1_1ResponseBuilder()
            .status(HttpStatusCode::SwitchingProtocol_101)
            .header("Upgrade", "websocket")
            .header("Connection", "Upgrade")
            .header("Sec-WebSocket-Accept", generateWebSocketAcceptKey(clientKey))
            .header("Server", GALAY_SERVER)
            .buildMove();
    }
#else
    // WebSocket 功能禁用时的占位实现
    std::string HttpHelper::generateWebSocketAcceptKey(const std::string& clientKey)
    {
        (void)clientKey;
        return "";
    }

    HttpResponse HttpHelper::createWebSocketUpgradeResponse(const std::string& clientKey)
    {
        (void)clientKey;
        return defaultNotImplemented();
    }
#endif
}
