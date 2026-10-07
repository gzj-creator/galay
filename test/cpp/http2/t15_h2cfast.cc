/**
 * @file T51-H2cServerFastPath.cc
 * @brief HTTP/2 h2c server raw frame-view fast-path contract
 */

#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#undef private
#include <galay/cpp/galay-http2/protoc/http2_frame.h>

#include <cassert>
#include <iostream>
#include <string_view>

using namespace galay::http2;

template<typename T>
concept HasReadFrameViewsBatch = requires(T& conn) {
    { conn.read_frame_views_batch() };
    { conn.read_frame_views_batch(8) };
};

template<typename T>
concept HasRawFrameViewSurface = requires(const T& view) {
    { view.header };
    { view.bytes() } -> std::same_as<std::string_view>;
    { view.payload() } -> std::same_as<std::string_view>;
    { view.stream_id() } -> std::same_as<uint32_t>;
    { view.is_headers() } -> std::same_as<bool>;
    { view.is_data() } -> std::same_as<bool>;
    { view.is_continuation() } -> std::same_as<bool>;
};

template<typename T>
concept HasFastDispatchHelper = requires(T* mgr, Http2RawFrameView view) {
    { mgr->try_dispatch_server_active_frame_view(std::move(view)) } -> std::same_as<bool>;
};

int main() {
    static_assert(HasReadFrameViewsBatch<Http2Conn>,
                  "Http2Conn must expose readFrameViewsBatch(max_frames)");

    static_assert(HasRawFrameViewSurface<Http2RawFrameView>,
                  "Http2RawFrameView must expose header, bytes(), payload(), streamId() and type helpers");

    static_assert(HasFastDispatchHelper<Http2StreamManagerImpl<galay::async::AsyncTcpSocket>>,
                  "Http2StreamManagerImpl<AsyncTcpSocket> must expose tryDispatchServerActiveFrameView(raw_view)");

    galay::async::AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    const auto headers = Http2FrameBuilder::headers_bytes(1, "abc", false, true);
    const auto data = Http2FrameBuilder::data_bytes(1, "body", true);

    conn.feed_data(headers.data(), headers.size());
    conn.feed_data(data.data(), data.size());

    auto awaitable = conn.read_frame_views_batch(8);
    assert(awaitable.await_ready() && "Buffered raw frame views should be ready immediately");

    auto result = awaitable.await_resume();
    assert(result.has_value() && "Buffered raw frame-view parse should succeed");
    assert(result->size() == 2 && "Expected HEADERS + DATA views");

    const auto& header_view = result->at(0);
    assert(header_view.is_headers());
    assert(!header_view.is_data());
    assert(header_view.stream_id() == 1);
    assert(header_view.payload() == "abc");
    assert(header_view.bytes() == headers);

    const auto& data_view = result->at(1);
    assert(data_view.is_data());
    assert(!data_view.is_headers());
    assert(data_view.stream_id() == 1);
    assert(data_view.payload() == "body");
    assert(data_view.bytes() == data);

    std::cout << "T51-H2cServerFastPath PASS\n";
    return 0;
}
