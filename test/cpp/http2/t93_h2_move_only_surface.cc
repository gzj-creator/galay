/**
 * @file t93_h2_move_only_surface.cc
 * @brief HTTP/2 owning state must move by default and clone explicitly.
 */

#include <galay/cpp/galay-http2/kernel/http2_conn.h>
#include <galay/cpp/galay-http2/kernel/http2_stream.h>
#include <galay/cpp/galay-http2/protoc/http2_frame.h>
#include <galay/cpp/galay-http2/protoc/http2_hpack.h>

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace galay::http2;

namespace {

#ifdef GALAY_SSL_FEATURE_ENABLED
struct SslReadContractState {
    using ResultType = std::expected<int, Http2ErrorCode>;

    bool hasResult() const { return true; }
    ResultType takeResult() { return 0; }
    bool parseFromRingBuffer() { return false; }
    bool completeIfClosing() { return false; }
    bool prepareRecvWindow(char*& buffer, size_t& length) {
        buffer = nullptr;
        length = 0;
        return false;
    }
    void setSslRecvError(const galay::ssl::SslError&) {}
    void setProtocolError(Http2ErrorCode, std::string_view) {}
    void onBytesReceived(size_t) {}
};
#endif

template<typename T>
concept HasClone = requires(const T& value) {
    value.clone();
};

template<typename T>
concept HasConcreteClone = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

void assertFrameClonePreservesDynamicTypeAndPayload()
{
    Http2HeadersFrame headers;
    headers.header().stream_id = 3;
    headers.setEndHeaders(true);
    headers.setHeaderBlock("encoded-headers");

    Http2HeadersFrame cloned_headers = headers.clone();
    assert(&cloned_headers != &headers);
    assert(cloned_headers.isHeaders());
    assert(cloned_headers.streamId() == 3);
    assert(cloned_headers.isEndHeaders());
    assert(cloned_headers.headerBlock() == "encoded-headers");

    headers.setHeaderBlock("mutated");
    assert(cloned_headers.headerBlock() == "encoded-headers");
}

const std::string* findHeader(const std::vector<Http2HeaderField>& headers,
                              const std::string& name)
{
    for (const auto& header : headers) {
        if (header.name == name) {
            return &header.value;
        }
    }
    return nullptr;
}

void assertBodyRequestResponseCloneDeepCopies()
{
    Http2ChunkedBody body;
    body.append(std::string("first"));
    body.append(std::string("-second"));

    auto body_clone = body.clone();
    assert(body_clone.coalesce() == "first-second");
    body.append(std::string("-original"));
    body_clone.append(std::string("-clone"));
    assert(body.coalesce() == "first-second-original");
    assert(body_clone.coalesce() == "first-second-clone");

    Http2Request request;
    request.method = "POST";
    request.scheme = "https";
    request.authority = "example.test";
    request.path = "/clone";
    request.headers.push_back({"x-request", "one"});
    request.setCommonHeader(galay::http2::detail::Http2RequestCommonHeaderIndex::ContentType,
                            "text/plain");
    request.body.append(std::string("body"));

    auto request_clone = request.clone();
    assert(request_clone.method == "POST");
    assert(request_clone.getHeader("content-type") == "text/plain");
    assert(request_clone.coalescedBody() == "body");

    request.headers[0].value = "mutated";
    request.body.append(std::string("-original"));
    request_clone.headers[0].value = "clone";
    request_clone.body.append(std::string("-clone"));
    assert(request.headers[0].value == "mutated");
    assert(request_clone.headers[0].value == "clone");
    assert(request.coalescedBody() == "body-original");
    assert(request_clone.coalescedBody() == "body-clone");

    Http2Response response;
    response.setStatus(201);
    response.setHeader("x-response", "one");
    response.setBody("payload");

    auto response_clone = response.clone();
    response.setHeader("x-response", "mutated");
    response.setBody("original");
    response_clone.setHeader("x-response", "clone");
    response_clone.body.append("-copy");
    assert(response.status == 201);
    const std::string* response_header = findHeader(response.headers, "x-response");
    const std::string* response_clone_header = findHeader(response_clone.headers, "x-response");
    assert(response_header != nullptr);
    assert(response_clone_header != nullptr);
    assert(*response_header == "mutated");
    assert(response.body == "original");
    assert(response_clone.status == 201);
    assert(*response_clone_header == "clone");
    assert(response_clone.body == "payload-copy");
}

void assertHpackStateCloneDeepCopies()
{
    HpackDynamicTable table;
    table.add({"x-one", "1"});

    auto table_clone = table.clone();
    assert(table_clone.count() == table.count());
    assert(table_clone.currentSize() == table.currentSize());
    assert(table_clone.get(0) != nullptr);
    assert(table_clone.get(0)->value == "1");

    table.add({"x-two", "2"});
    assert(table.count() == 2);
    assert(table_clone.count() == 1);
    assert(table_clone.get(0)->name == "x-one");

    HpackEncoder encoder;
    encoder.dynamicTable().add({"x-encoder", "1"});
    encoder.setMaxTableSize(128);

    auto encoder_clone = encoder.clone();
    assert(encoder_clone.dynamicTable().count() == encoder.dynamicTable().count());
    assert(encoder_clone.dynamicTable().maxSize() == encoder.dynamicTable().maxSize());
    encoder.dynamicTable().add({"x-encoder-original", "2"});
    assert(encoder.dynamicTable().count() == 2);
    assert(encoder_clone.dynamicTable().count() == 1);

    HpackDecoder decoder;
    decoder.dynamicTable().add({"x-decoder", "1"});
    decoder.setMaxHeaderListSize(64);

    auto decoder_clone = decoder.clone();
    assert(decoder_clone.dynamicTable().count() == decoder.dynamicTable().count());
    assert(decoder_clone.maxHeaderListSize() == 64);
    decoder.dynamicTable().add({"x-decoder-original", "2"});
    assert(decoder.dynamicTable().count() == 2);
    assert(decoder_clone.dynamicTable().count() == 1);
}

} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<Http2Frame>);
    static_assert(!std::is_copy_assignable_v<Http2Frame>);
    static_assert(!HasClone<Http2Frame>);

    static_assert(HasConcreteClone<Http2DataFrame>);
    static_assert(HasConcreteClone<Http2HeadersFrame>);
    static_assert(HasConcreteClone<Http2PriorityFrame>);
    static_assert(HasConcreteClone<Http2RstStreamFrame>);
    static_assert(HasConcreteClone<Http2SettingsFrame>);
    static_assert(HasConcreteClone<Http2PushPromiseFrame>);
    static_assert(HasConcreteClone<Http2PingFrame>);
    static_assert(HasConcreteClone<Http2GoAwayFrame>);
    static_assert(HasConcreteClone<Http2WindowUpdateFrame>);
    static_assert(HasConcreteClone<Http2ContinuationFrame>);

    static_assert(!std::is_copy_constructible_v<Http2DataFrame>);
    static_assert(!std::is_copy_constructible_v<Http2HeadersFrame>);
    static_assert(!std::is_copy_constructible_v<Http2PriorityFrame>);
    static_assert(!std::is_copy_constructible_v<Http2RstStreamFrame>);
    static_assert(!std::is_copy_constructible_v<Http2SettingsFrame>);
    static_assert(!std::is_copy_constructible_v<Http2PushPromiseFrame>);
    static_assert(!std::is_copy_constructible_v<Http2PingFrame>);
    static_assert(!std::is_copy_constructible_v<Http2GoAwayFrame>);
    static_assert(!std::is_copy_constructible_v<Http2WindowUpdateFrame>);
    static_assert(!std::is_copy_constructible_v<Http2ContinuationFrame>);

    static_assert(!std::is_copy_constructible_v<Http2OutgoingFrame>);
    static_assert(!std::is_copy_assignable_v<Http2OutgoingFrame>);
    static_assert(std::is_move_constructible_v<Http2OutgoingFrame>);
    static_assert(std::is_move_assignable_v<Http2OutgoingFrame>);
    static_assert(!HasClone<Http2OutgoingFrame>);

    static_assert(!std::is_copy_constructible_v<Http2ChunkedBody>);
    static_assert(!std::is_copy_assignable_v<Http2ChunkedBody>);
    static_assert(std::is_move_constructible_v<Http2ChunkedBody>);
    static_assert(std::is_move_assignable_v<Http2ChunkedBody>);
    static_assert(HasClone<Http2ChunkedBody>);

    static_assert(!std::is_copy_constructible_v<Http2Request>);
    static_assert(!std::is_copy_assignable_v<Http2Request>);
    static_assert(std::is_move_constructible_v<Http2Request>);
    static_assert(std::is_move_assignable_v<Http2Request>);
    static_assert(HasClone<Http2Request>);

    static_assert(!std::is_copy_constructible_v<Http2Response>);
    static_assert(!std::is_copy_assignable_v<Http2Response>);
    static_assert(std::is_move_constructible_v<Http2Response>);
    static_assert(std::is_move_assignable_v<Http2Response>);
    static_assert(HasClone<Http2Response>);

    static_assert(!std::is_copy_constructible_v<HpackDynamicTable>);
    static_assert(!std::is_copy_assignable_v<HpackDynamicTable>);
    static_assert(std::is_move_constructible_v<HpackDynamicTable>);
    static_assert(std::is_move_assignable_v<HpackDynamicTable>);
    static_assert(HasClone<HpackDynamicTable>);

    static_assert(!std::is_copy_constructible_v<HpackEncoder>);
    static_assert(!std::is_copy_assignable_v<HpackEncoder>);
    static_assert(std::is_move_constructible_v<HpackEncoder>);
    static_assert(std::is_move_assignable_v<HpackEncoder>);
    static_assert(HasClone<HpackEncoder>);

    static_assert(!std::is_copy_constructible_v<HpackDecoder>);
    static_assert(!std::is_copy_assignable_v<HpackDecoder>);
    static_assert(std::is_move_constructible_v<HpackDecoder>);
    static_assert(std::is_move_assignable_v<HpackDecoder>);
    static_assert(HasClone<HpackDecoder>);

    static_assert(std::is_copy_constructible_v<Http2HeaderField>);
    static_assert(std::is_copy_assignable_v<Http2HeaderField>);
    static_assert(std::is_copy_constructible_v<Http2FrameHeader>);
    static_assert(std::is_copy_assignable_v<Http2FrameHeader>);
    static_assert(std::is_copy_constructible_v<Http2ErrorCode>);
    static_assert(std::is_copy_assignable_v<Http2ErrorCode>);
    static_assert(std::is_copy_constructible_v<Http2Error>);
    static_assert(std::is_copy_assignable_v<Http2Error>);
#ifdef GALAY_SSL_FEATURE_ENABLED
    static_assert(std::is_constructible_v<Http2Error, const galay::ssl::SslError&>);
    static_assert(std::same_as<
                  galay::http2::detail::Http2SslReadMachine<SslReadContractState>::result_type,
                  std::expected<int, Http2Error>>);
    static_assert(std::same_as<
                  galay::http2::detail::Http2SslWriteMachine::result_type,
                  std::expected<bool, Http2Error>>);
#endif
    static_assert(std::is_copy_constructible_v<Http2Settings>);
    static_assert(std::is_copy_assignable_v<Http2Settings>);
    static_assert(std::is_copy_constructible_v<Http2RuntimeConfig>);
    static_assert(std::is_copy_assignable_v<Http2RuntimeConfig>);

    assertFrameClonePreservesDynamicTypeAndPayload();
    assertBodyRequestResponseCloneDeepCopies();
    assertHpackStateCloneDeepCopies();

    std::cout << "t93_h2_move_only_surface PASS\n";
    return 0;
}
