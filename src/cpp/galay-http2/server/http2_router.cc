#include "http2_router.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace galay::http2::server_detail {
namespace {

std::expected<http::HttpRequest, http::HttpError> normalize_request(http2::Http2Request& source)
{
    http::HttpRequest request;
    auto& header = request.header();
    header.method() = http::string_to_http_method(source.method);
    header.version() = http::HttpVersion::HttpVersion_2_0;
    if (std::any_of(source.path.begin(), source.path.end(), [](unsigned char character) {
            return character <= 0x20 || character == 0x7f;
        })) return std::unexpected(http::HttpError{http::kBadRequest, "invalid HTTP/2 :path"});
    if (const auto error = header.set_request_target(source.path); error != http::kNoError) {
        return std::unexpected(http::HttpError{error, "HTTP/2 :path"});
    }
    auto& headers = header.header_pairs();
    if (!source.authority.empty()) {
        const auto added = headers.add_header_pair("Host", source.authority);
        if (added != http::kNoError) return std::unexpected(http::HttpError{added, "HTTP/2 :authority"});
    }
    for (const auto& field : source.headers) {
        const auto added = headers.add_header_pair(field.name, field.value);
        if (added != http::kNoError) return std::unexpected(http::HttpError{added, "HTTP/2 header " + field.name});
    }
    // StreamManager stores these fields in its common-header cache, not headers.
    constexpr std::array names{"content-length", "content-type", "accept-encoding", "user-agent"};
    for (const auto* name : names) {
        const auto value = source.get_header(name);
        if (!value.empty()) {
            const auto added = headers.add_header_pair(name, value);
            if (added != http::kNoError) return std::unexpected(http::HttpError{added, "HTTP/2 header " + std::string(name)});
        }
    }
    request.set_body_str(source.take_coalesced_body());
    return request;
}

bool valid_content_length(const http2::Http2Request& request)
{
    const auto value = request.get_header("content-length");
    if (value.empty()) return true;
    std::size_t length = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), length);
    return converted.ec == std::errc{} && converted.ptr == value.data() + value.size() && length == request.body_size();
}

kernel::Task<void> send_response(http2::Http2Stream::ptr stream, http::HttpResponse response, bool head)
{
    if (stream->is_frame_queue_closed()) co_return;
    const int status = static_cast<int>(response.header().code());
    std::vector<http2::Http2HeaderField> headers{{":status", std::to_string(status)}};
    response.header().header_pairs().for_each_header([&](std::string_view name, std::string_view value) {
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : static_cast<char>(character);
        });
        if (lower == "connection" || lower == "keep-alive" || lower == "proxy-connection" ||
            lower == "transfer-encoding" || lower == "upgrade" || lower == "content-length") return;
        headers.push_back({std::move(lower), std::string(value)});
    });
    std::string body = response.get_body_str();
    const bool empty = head || status == 204 || status == 205 || body.empty();
    if (!head && status != 204) headers.push_back({"content-length", std::to_string(empty ? 0 : body.size())});
    const auto sent = empty ? co_await stream->reply_header(headers, true)
                            : co_await stream->reply_headers_and_data(headers, std::move(body));
    if (!sent && !stream->is_frame_queue_closed()) {
        HTTP_LOG_ERROR("[api-h2] [send-fail]", "stream={} error={}", stream->stream_id(), sent.error().message());
        const auto reset = co_await stream->reply_rst(http2::Http2ErrorCode::InternalError);
        if (!reset) HTTP_LOG_WARN("[api-h2] [reset-fail]", "stream={} error={}", stream->stream_id(), reset.error().message());
    }
}

} // namespace

kernel::Task<void> execute_http2_route(std::shared_ptr<http::HttpRouter> router, http2::Http2Stream::ptr stream)
{
    // Keep both the route table and stream alive through receive, handler and send.
    stream->set_frame_queue_enabled(false);
    const auto complete = co_await stream->wait_request_complete();
    if (!complete || stream->is_frame_queue_closed() || !stream->is_end_stream_received()) co_return;
    if (!valid_content_length(stream->request())) {
        HTTP_LOG_WARN("[api-h2] [content-length-mismatch]", "stream={} declared={} received={}",
            stream->stream_id(), stream->request().get_header("content-length"), stream->request().body_size());
        const auto reset = co_await stream->reply_rst(http2::Http2ErrorCode::ProtocolError);
        if (!reset) HTTP_LOG_WARN("[api-h2] [reset-fail]", "stream={} error={}", stream->stream_id(), reset.error().message());
        co_return;
    }
    const bool head = stream->request().method == "HEAD";
    auto request = normalize_request(stream->request());
    http::HttpResponseResult response = std::unexpected(http::HttpError{http::kInternalError});
    if (!request) {
        http::HttpResponse invalid;
        invalid.header().code() = http::HttpStatusCode::BadRequest_400;
        const auto added = invalid.header().header_pairs().add_header_pair("Content-Type", "text/plain");
        if (added != http::kNoError) response = std::unexpected(http::HttpError{added});
        else {
            if (!head) invalid.set_body_str(request.error().message());
            response = std::move(invalid);
        }
    } else {
        auto match = router->find_handler(request->header().method(), request->header().uri());
        request->set_route_params(std::move(match.params));
        if (!match.request_handler) {
            http::HttpResponse missing;
            missing.header().code() = http::HttpStatusCode::NotFound_404;
            const auto added = missing.header().header_pairs().add_header_pair("Content-Type", "text/plain");
            if (added != http::kNoError) response = std::unexpected(http::HttpError{added});
            else {
                if (!head) missing.set_body_str("404 Not Found");
                response = std::move(missing);
            }
        } else {
            auto handled = co_await (*match.request_handler)(std::move(*request));
            if (handled) response = std::move(*handled);
            else response = std::unexpected(http::HttpError{http::kInternalError, std::string(handled.error().message())});
        }
    }
    // Reset/connection close may have occurred while the application suspended.
    if (stream->is_frame_queue_closed()) co_return;
    if (!response) {
        HTTP_LOG_ERROR("[api-h2] [handler-fail]", "stream={} error={}", stream->stream_id(), response.error().message());
        const auto reset = co_await stream->reply_rst(http2::Http2ErrorCode::InternalError);
        if (!reset) HTTP_LOG_WARN("[api-h2] [reset-fail]", "stream={} error={}", stream->stream_id(), reset.error().message());
        co_return;
    }
    const auto sent = co_await send_response(stream, std::move(*response), head);
    if (!sent) HTTP_LOG_ERROR("[api-h2] [send-task-fail]", "stream={} error={}", stream->stream_id(), sent.error().message());
}

} // namespace galay::http2::server_detail
