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
    std::declval<TcpHttpWriter&>().sendResponse(std::declval<galay::http::HttpResponse&&>()));
using RvalueRequestOperation = decltype(
    std::declval<TcpHttpWriter&>().sendRequest(std::declval<galay::http::HttpRequest&&>()));
using LvalueResponseHeaderOperation = decltype(
    std::declval<TcpHttpWriter&>().sendHeader(std::declval<galay::http::HttpResponseHeader&>()));
using LvalueRequestHeaderOperation = decltype(
    std::declval<TcpHttpWriter&>().sendHeader(std::declval<galay::http::HttpRequestHeader&>()));

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
    response.setBodyStr(std::string(kLvalueResponseBody));

    // Only synchronous layout preparation is under test; no socket send is started.
    (void) writer.sendResponse(response);
    if (response.bodyStr() != kLvalueResponseBody) {
        std::cerr << "[T90] lvalue response body should remain unchanged\n";
        return 1;
    }
    writer.updateRemainingWritev(writer.getRemainingBytes());

    static constexpr std::string_view kRequestBody = "rvalue-request-body";
    HttpRequest request;
    request.setBodyStr(std::string(kRequestBody));

    (void) writer.sendRequest(std::move(request));
    const iovec* request_iovecs = writer.getIovecsData();
    if (writer.getIovecsCount() != 2 || request_iovecs == nullptr ||
        std::string_view(static_cast<const char*>(request_iovecs[1].iov_base),
                         request_iovecs[1].iov_len) != kRequestBody) {
        std::cerr << "[T90] rvalue request body was not transferred to writer storage\n";
        return 1;
    }
    writer.updateRemainingWritev(writer.getRemainingBytes());

    HttpResponseHeader response_header;
    const std::string expected_response_header = response_header.toString();
    (void) writer.sendHeader(response_header);
    if (std::string_view(writer.bufferData(), writer.getRemainingBytes()) !=
        expected_response_header) {
        std::cerr << "[T90] lvalue response header serialization mismatch\n";
        return 1;
    }
    writer.updateRemaining(writer.getRemainingBytes());

    HttpRequestHeader request_header;
    const std::string expected_request_header = request_header.toString();
    (void) writer.sendHeader(request_header);
    if (std::string_view(writer.bufferData(), writer.getRemainingBytes()) !=
        expected_request_header) {
        std::cerr << "[T90] lvalue request header serialization mismatch\n";
        return 1;
    }

    std::cout << "T90-HttpWriterOverloads PASS\n";
    return 0;
}
