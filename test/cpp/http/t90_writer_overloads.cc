#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <galay/cpp/galay-http/kernel/http_writer.h>
#include <galay/cpp/galay-kernel/async/async_tcp.h>

using TcpHttpWriter = galay::http::HttpWriterImpl<galay::async::AsyncTcpSocket>;
using SendResult = std::expected<bool, galay::http::HttpError>;

template<typename Operation>
using AwaitResult = decltype(std::declval<Operation&&>().await_resume());

using RvalueResponseOperation = decltype(
    std::declval<TcpHttpWriter&>().send_response(std::declval<galay::http::HttpResponse&&>()));
using RvalueRequestOperation = decltype(
    std::declval<TcpHttpWriter&>().send_request(std::declval<galay::http::HttpRequest&&>()));
using LvalueResponseHeaderOperation = decltype(
    std::declval<TcpHttpWriter&>().send_header(std::declval<galay::http::HttpResponseHeader&>()));
using LvalueRequestHeaderOperation = decltype(
    std::declval<TcpHttpWriter&>().send_header(std::declval<galay::http::HttpRequestHeader&>()));

static_assert(std::is_same_v<AwaitResult<RvalueResponseOperation>, SendResult>);
static_assert(std::is_same_v<AwaitResult<RvalueRequestOperation>, SendResult>);
static_assert(std::is_same_v<AwaitResult<LvalueResponseHeaderOperation>, SendResult>);
static_assert(std::is_same_v<AwaitResult<LvalueRequestHeaderOperation>, SendResult>);

int main()
{
    using namespace galay::async;
    using namespace galay::http;

    AsyncTcpSocket socket(IPType::IPV4);
    TcpHttpWriter writer(HttpWriterSetting(), socket);

    static constexpr std::string_view kLvalueResponseBody =
        "lvalue-response-body-must-remain-owned-by-response";
    HttpResponse response;
    response.set_body_str(std::string(kLvalueResponseBody));

    // Only synchronous layout preparation is under test; no socket send is started.
    (void) writer.send_response(response);
    if (response.body_str() != kLvalueResponseBody) {
        std::cerr << "[T90] lvalue response body should remain unchanged\n";
        return 1;
    }
    writer.update_remaining_writev(writer.get_remaining_bytes());

    static constexpr std::string_view kRequestBody = "rvalue-request-body";
    HttpRequest request;
    request.set_body_str(std::string(kRequestBody));

    (void) writer.send_request(std::move(request));
    const iovec* request_iovecs = writer.get_iovecs_data();
    if (writer.get_iovecs_count() != 2 || request_iovecs == nullptr ||
        std::string_view(static_cast<const char*>(request_iovecs[1].iov_base),
                         request_iovecs[1].iov_len) != kRequestBody) {
        std::cerr << "[T90] rvalue request body was not transferred to writer storage\n";
        return 1;
    }
    writer.update_remaining_writev(writer.get_remaining_bytes());

    HttpResponseHeader response_header;
    const std::string expected_response_header = response_header.to_string();
    (void) writer.send_header(response_header);
    if (std::string_view(writer.buffer_data(), writer.get_remaining_bytes()) !=
        expected_response_header) {
        std::cerr << "[T90] lvalue response header serialization mismatch\n";
        return 1;
    }
    writer.update_remaining(writer.get_remaining_bytes());

    HttpRequestHeader request_header;
    const std::string expected_request_header = request_header.to_string();
    (void) writer.send_header(request_header);
    if (std::string_view(writer.buffer_data(), writer.get_remaining_bytes()) !=
        expected_request_header) {
        std::cerr << "[T90] lvalue request header serialization mismatch\n";
        return 1;
    }

    std::cout << "T90-HttpWriterOverloads PASS\n";
    return 0;
}
