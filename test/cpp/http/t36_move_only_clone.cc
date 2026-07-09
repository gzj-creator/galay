#include <cassert>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/uio.h>

#include <galay/cpp/galay-http/builder/http_builder.h>
#include <galay/cpp/galay-http/protoc/http_body.h>
#include <galay/cpp/galay-http/protoc/http_chunk.h>
#include <galay/cpp/galay-http/protoc/http_header.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/protoc/http_response.h>

using namespace galay::http;

template <typename T>
concept HasValueClone = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

template <typename T>
constexpr void assert_move_only_clone_type()
{
    static_assert(!std::is_copy_constructible_v<T>);
    static_assert(!std::is_copy_assignable_v<T>);
    static_assert(std::is_move_constructible_v<T>);
    static_assert(std::is_move_assignable_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_move_assignable_v<T>);
    static_assert(HasValueClone<T>);
}

static std::vector<iovec> make_iovecs(std::string& data)
{
    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = data.data();
    iovecs[0].iov_len = data.size();
    return iovecs;
}

static void test_move_only_clone_traits()
{
    assert_move_only_clone_type<HeaderPair>();
    assert_move_only_clone_type<HttpRequestHeader>();
    assert_move_only_clone_type<HttpResponseHeader>();
    assert_move_only_clone_type<HttpRequest>();
    assert_move_only_clone_type<HttpResponse>();
    assert_move_only_clone_type<ChunkParser>();
    assert_move_only_clone_type<PlainBody>();
    assert_move_only_clone_type<Http1_1RequestBuilder>();
    assert_move_only_clone_type<Http1_1ResponseBuilder>();

    static_assert(std::is_copy_constructible_v<HttpMethod>);
    static_assert(std::is_copy_assignable_v<HttpMethod>);
    static_assert(std::is_copy_constructible_v<HttpVersion>);
    static_assert(std::is_copy_assignable_v<HttpVersion>);
    static_assert(std::is_copy_constructible_v<HttpStatusCode>);
    static_assert(std::is_copy_assignable_v<HttpStatusCode>);
    static_assert(std::is_copy_constructible_v<HttpErrorCode>);
    static_assert(std::is_copy_assignable_v<HttpErrorCode>);
}

static void test_header_pair_clone_is_independent()
{
    HeaderPair original(HeaderPair::Mode::ServerSide);
    assert(original.addHeaderPair("Host", "example.com") == kNoError);
    assert(original.addHeaderPair("X-Trace", "before") == kNoError);

    HeaderPair cloned = original.clone();
    assert(original.addHeaderPair("Host", "changed.example") == kNoError);
    assert(original.addHeaderPair("X-Trace", "after") == kNoError);

    assert(cloned.getValue("host") == "example.com");
    assert(cloned.getValue("x-trace") == "before");
    assert(original.getValue("host") == "example.com, changed.example");
    assert(original.getValue("x-trace") == "after");
}

static void test_request_header_clone_is_independent()
{
    HttpRequestHeader original;
    original.method() = HttpMethod::POST;
    original.uri() = "/submit?name=galay";
    original.version() = HttpVersion::HttpVersion_1_1;
    assert(original.headerPairs().addHeaderPair("Content-Type", "text/plain") == kNoError);

    HttpRequestHeader cloned = original.clone();
    original.method() = HttpMethod::GET;
    original.uri() = "/changed";
    assert(original.headerPairs().addHeaderPair("Content-Type", "application/json") == kNoError);

    assert(cloned.method() == HttpMethod::POST);
    assert(cloned.uri() == "/submit?name=galay");
    assert(cloned.headerPairs().getValue("content-type") == "text/plain");
}

static void test_response_header_clone_is_independent()
{
    HttpResponseHeader original;
    original.version() = HttpVersion::HttpVersion_1_1;
    original.code() = HttpStatusCode::Created_201;
    assert(original.headerPairs().addHeaderPair("Content-Type", "text/plain") == kNoError);

    HttpResponseHeader cloned = original.clone();
    original.code() = HttpStatusCode::InternalServerError_500;
    assert(original.headerPairs().addHeaderPair("Content-Type", "application/json") == kNoError);

    assert(cloned.code() == HttpStatusCode::Created_201);
    assert(cloned.headerPairs().getValue("content-type") == "text/plain");
}

static void test_plain_body_clone_is_independent()
{
    PlainBody original;
    std::string body = "plain-body";
    assert(original.fromString(std::move(body)));

    PlainBody cloned = original.clone();
    std::string changed = "changed-body";
    assert(original.fromString(std::move(changed)));

    assert(original.toString() == "changed-body");
    assert(cloned.toString() == "plain-body");
}

static void test_request_clone_is_independent()
{
    HttpRequest original;
    original.header().method() = HttpMethod::POST;
    original.header().uri() = "/clone";
    assert(original.header().headerPairs().addHeaderPair("X-Trace", "before") == kNoError);
    original.setBodyStr(std::string("request-body"));
    original.setRouteParams({{"id", "42"}});

    HttpRequest cloned = original.clone();
    original.header().uri() = "/changed";
    assert(original.header().headerPairs().addHeaderPair("X-Trace", "after") == kNoError);
    original.setBodyStr(std::string("changed-body"));
    original.setRouteParams({{"id", "99"}});

    assert(cloned.header().method() == HttpMethod::POST);
    assert(cloned.header().uri() == "/clone");
    assert(cloned.header().headerPairs().getValue("x-trace") == "before");
    assert(cloned.bodyStr() == "request-body");
    assert(cloned.getRouteParam("id") == "42");
}

static void test_response_clone_is_independent()
{
    HttpResponse original;
    original.header().code() = HttpStatusCode::OK_200;
    assert(original.header().headerPairs().addHeaderPair("X-Trace", "before") == kNoError);
    original.setBodyStr(std::string("response-body"));

    HttpResponse cloned = original.clone();
    original.header().code() = HttpStatusCode::BadGateway_502;
    assert(original.header().headerPairs().addHeaderPair("X-Trace", "after") == kNoError);
    original.setBodyStr(std::string("changed-body"));

    assert(cloned.header().code() == HttpStatusCode::OK_200);
    assert(cloned.header().headerPairs().getValue("x-trace") == "before");
    assert(cloned.bodyStr() == "response-body");
}

static void test_chunk_parser_clone_keeps_independent_state()
{
    ChunkParser original;
    std::string first = "5\r\nHe";
    std::string original_body;
    auto first_iovecs = make_iovecs(first);
    auto first_result = original.parse(first_iovecs, original_body);
    assert(first_result.has_value());
    assert(!first_result->first);
    assert(first_result->second == first.size());
    assert(original_body == "He");

    ChunkParser cloned = original.clone();

    std::string original_tail = "llo\r\n0\r\n\r\n";
    auto original_tail_iovecs = make_iovecs(original_tail);
    auto original_result = original.parse(original_tail_iovecs, original_body);
    assert(original_result.has_value());
    assert(original_result->first);
    assert(original_body == "Hello");

    std::string cloned_tail = "y!!\r\n0\r\n\r\n";
    std::string cloned_body = "He";
    auto cloned_tail_iovecs = make_iovecs(cloned_tail);
    auto cloned_result = cloned.parse(cloned_tail_iovecs, cloned_body);
    assert(cloned_result.has_value());
    assert(cloned_result->first);
    assert(cloned_body == "Hey!!");
}

static void test_builder_clone_is_independent()
{
    HttpRequest chained_request = Http1_1RequestBuilder::post("/submit")
        .header("X-Trace", "before")
        .body("request-body")
        .clone()
        .build();
    assert(chained_request.header().uri() == "/submit");
    assert(chained_request.header().headerPairs().getValue("X-Trace") == "before");
    assert(chained_request.bodyStr() == "request-body");

    Http1_1RequestBuilder source_request_builder = Http1_1RequestBuilder::post("/submit");
    source_request_builder.header("X-Trace", "before").body("request-body");
    Http1_1RequestBuilder cloned_request_builder = source_request_builder.clone();
    source_request_builder.header("X-Trace", "after").body("changed-body");

    HttpRequest request = cloned_request_builder.build();
    assert(request.header().uri() == "/submit");
    assert(request.header().headerPairs().getValue("X-Trace") == "before");
    assert(request.bodyStr() == "request-body");

    HttpResponse response = Http1_1ResponseBuilder::ok()
        .header("X-Trace", "before")
        .body("response-body")
        .clone()
        .build();
    assert(response.header().code() == HttpStatusCode::OK_200);
    assert(response.header().headerPairs().getValue("x-trace") == "before");
    assert(response.bodyStr() == "response-body");
}

int main()
{
    test_move_only_clone_traits();
    test_header_pair_clone_is_independent();
    test_request_header_clone_is_independent();
    test_response_header_clone_is_independent();
    test_plain_body_clone_is_independent();
    test_request_clone_is_independent();
    test_response_clone_is_independent();
    test_chunk_parser_clone_keeps_independent_state();
    test_builder_clone_is_independent();

    std::cout << "t36 move-only clone checks passed\n";
    return 0;
}
