/**
 * @file T51-H2cServerFastPath.cc
 * @brief HTTP/2 h2c server raw frame-view fast-path contract
 */

#include <sstream>

#define private public
#include "galay-http2/kernel/stream_mgr.h"
#undef private
#include "galay-http2/protoc/http2_frame.h"

#include <cassert>
#include <iostream>
#include <string_view>

using namespace galay::http2;

template<typename T>
concept HasReadFrameViewsBatch = requires(T& conn) {
    { conn.readFrameViewsBatch() };
    { conn.readFrameViewsBatch(8) };
};

template<typename T>
concept HasRawFrameViewSurface = requires(const T& view) {
    { view.header };
    { view.bytes() } -> std::same_as<std::string_view>;
    { view.payload() } -> std::same_as<std::string_view>;
    { view.streamId() } -> std::same_as<uint32_t>;
    { view.isHeaders() } -> std::same_as<bool>;
    { view.isData() } -> std::same_as<bool>;
    { view.isContinuation() } -> std::same_as<bool>;
};

template<typename T>
concept HasFastDispatchHelper = requires(T* mgr, Http2RawFrameView view) {
    { mgr->tryDispatchServerActiveFrameView(std::move(view)) } -> std::same_as<bool>;
};

int main() {
    static_assert(HasReadFrameViewsBatch<Http2Conn>,
                  "Http2Conn must expose readFrameViewsBatch(max_frames)");

    static_assert(HasRawFrameViewSurface<Http2RawFrameView>,
                  "Http2RawFrameView must expose header, bytes(), payload(), streamId() and type helpers");

    static_assert(HasFastDispatchHelper<Http2StreamManagerImpl<galay::async::TcpSocket>>,
                  "Http2StreamManagerImpl<TcpSocket> must expose tryDispatchServerActiveFrameView(raw_view)");

    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    const auto headers = Http2FrameBuilder::headersBytes(1, "abc", false, true);
    const auto data = Http2FrameBuilder::dataBytes(1, "body", true);

    conn.feedData(headers.data(), headers.size());
    conn.feedData(data.data(), data.size());

    auto awaitable = conn.readFrameViewsBatch(8);
    assert(awaitable.await_ready() && "Buffered raw frame views should be ready immediately");

    auto result = awaitable.await_resume();
    assert(result.has_value() && "Buffered raw frame-view parse should succeed");
    assert(result->size() == 2 && "Expected HEADERS + DATA views");

    const auto& header_view = result->at(0);
    assert(header_view.isHeaders());
    assert(!header_view.isData());
    assert(header_view.streamId() == 1);
    assert(header_view.payload() == "abc");
    assert(header_view.bytes() == headers);

    const auto& data_view = result->at(1);
    assert(data_view.isData());
    assert(!data_view.isHeaders());
    assert(data_view.streamId() == 1);
    assert(data_view.payload() == "body");
    assert(data_view.bytes() == data);

    std::cout << "T51-H2cServerFastPath PASS\n";
    return 0;
}
