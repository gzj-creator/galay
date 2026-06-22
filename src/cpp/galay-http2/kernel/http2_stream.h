/**
 * @file http2_stream.h
 * @brief HTTP/2 流管理，封装请求/响应、帧收发和流状态
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HTTP/2 流的完整抽象，包括 Http2Request、Http2Response、
 *          Http2Stream 和 Http2StreamPool。管理流级别的帧收发、
 *          DATA 帧分片和流状态转换。
 */

#ifndef GALAY_HTTP2_STREAM_H
#define GALAY_HTTP2_STREAM_H

#include "../protoc/http2_base.h"
#include "../protoc/http2_frame.h"
#include "../protoc/http2_hpack.h"
#include "../protoc/http2_error.h"
#include "../../galay-kernel/concurrency/async_waiter.h"
#include "../../galay-kernel/concurrency/mpsc_channel.h"
#include "../../galay-kernel/concurrency/unsafe_channel.h"
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <charconv>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <array>
#include <bitset>
#include <iterator>
#include <type_traits>
#include <functional>
#include <utility>
#include <sys/uio.h>

namespace galay::http2
{

template<typename SocketType>
class Http2StreamManagerImpl;

template<typename SocketType>
class Http2ConnImpl;

class Http2ActiveStreamBatch;
class Http2StreamPool;

struct Http2OutgoingFrame {
    using Waiter = galay::kernel::AsyncWaiter<void>;
    using WaiterPtr = std::shared_ptr<Waiter>;

    Http2Frame::uptr frame;
    std::string serialized;
    std::array<char, kHttp2FrameHeaderLength> header_bytes{};
    std::string owned_payload;
    std::shared_ptr<const std::string> shared_payload;
    size_t shared_payload_offset = 0;
    size_t shared_payload_length = static_cast<size_t>(-1);
    bool segmented_packet = false;
    WaiterPtr waiter;

    Http2OutgoingFrame() = default;
    Http2OutgoingFrame(Http2Frame::uptr f, WaiterPtr w = nullptr)
        : frame(std::move(f))
        , waiter(std::move(w))
    {
    }

    Http2OutgoingFrame(std::string bytes, WaiterPtr w = nullptr)
        : serialized(std::move(bytes))
        , waiter(std::move(w))
    {
    }

    static Http2OutgoingFrame segmented(std::array<char, kHttp2FrameHeaderLength> header,
                                        std::string payload,
                                        WaiterPtr w = nullptr) {
        Http2OutgoingFrame frame;
        frame.header_bytes = std::move(header);
        frame.owned_payload = std::move(payload);
        frame.segmented_packet = true;
        frame.waiter = std::move(w);
        return frame;
    }

    static Http2OutgoingFrame segmentedShared(std::array<char, kHttp2FrameHeaderLength> header,
                                              std::shared_ptr<const std::string> payload,
                                              WaiterPtr w = nullptr) {
        Http2OutgoingFrame frame;
        frame.header_bytes = std::move(header);
        frame.shared_payload = std::move(payload);
        frame.segmented_packet = true;
        frame.waiter = std::move(w);
        return frame;
    }

    static Http2OutgoingFrame segmentedShared(std::array<char, kHttp2FrameHeaderLength> header,
                                              std::shared_ptr<const std::string> payload,
                                              size_t offset,
                                              size_t length,
                                              WaiterPtr w = nullptr) {
        Http2OutgoingFrame frame = segmentedShared(std::move(header), std::move(payload), std::move(w));
        frame.shared_payload_offset = offset;
        frame.shared_payload_length = length;
        return frame;
    }

    bool isSegmented() const {
        return segmented_packet;
    }

    bool isEmpty() const {
        return !segmented_packet && !frame && serialized.empty();
    }

    size_t serializedSize() const {
        if (segmented_packet) {
            return header_bytes.size() + payloadSize();
        }
        if (!serialized.empty()) {
            return serialized.size();
        }
        if (frame) {
            return kHttp2FrameHeaderLength + frame->header().length;
        }
        return 0;
    }

    void appendTo(std::string& bytes) const {
        if (segmented_packet) {
            bytes.append(header_bytes.data(), header_bytes.size());
            if (const char* payload = payloadData(); payload != nullptr) {
                bytes.append(payload, payloadSize());
            }
            return;
        }
        if (!serialized.empty()) {
            bytes.append(serialized);
            return;
        }
        if (frame) {
            bytes.append(frame->serialize());
        }
    }

    std::string flatten() const {
        std::string bytes;
        bytes.reserve(serializedSize());
        appendTo(bytes);
        return bytes;
    }

    size_t exportIovecs(std::array<struct iovec, 2>& iovecs) const {
        if (segmented_packet) {
            iovecs[0] = {
                .iov_base = const_cast<char*>(header_bytes.data()),
                .iov_len = header_bytes.size(),
            };
            if (const char* payload = payloadData(); payload != nullptr && payloadSize() > 0) {
                iovecs[1] = {
                    .iov_base = const_cast<char*>(payload),
                    .iov_len = payloadSize(),
                };
                return 2;
            }
            return 1;
        }

        if (!serialized.empty()) {
            iovecs[0] = {
                .iov_base = const_cast<char*>(serialized.data()),
                .iov_len = serialized.size(),
            };
            return 1;
        }

        return 0;
    }

private:
    const char* payloadData() const {
        if (shared_payload) {
            if (shared_payload_offset >= shared_payload->size()) {
                return nullptr;
            }
            return shared_payload->data() + shared_payload_offset;
        }
        if (!owned_payload.empty()) {
            return owned_payload.data();
        }
        return nullptr;
    }

    size_t payloadSize() const {
        if (shared_payload) {
            if (shared_payload_offset >= shared_payload->size()) {
                return 0;
            }
            const auto available = shared_payload->size() - shared_payload_offset;
            return std::min(available, shared_payload_length);
        }
        return owned_payload.size();
    }
};

/**
 * @brief HTTP/2 分块请求体
 */
struct Http2ChunkedBody
{
    bool empty() const { return m_total_bytes == 0; }
    size_t size() const { return m_total_bytes; }
    size_t chunkCount() const { return m_chunk_count; }

    void clear() {
        m_single_chunk.clear();
        m_chunks.clear();
        m_total_bytes = 0;
        m_chunk_count = 0;
        m_single_view_cache.clear();
    }

    void set(std::string data) {
        clear();
        append(std::move(data));
    }

    void append(const std::string& data) {
        if (data.empty()) {
            return;
        }
        m_total_bytes += data.size();
        if (m_chunk_count == 0) {
            m_single_chunk = data;
            m_chunk_count = 1;
            m_single_view_cache.clear();
            return;
        }

        ensureChunkVector();
        m_chunks.push_back(data);
        ++m_chunk_count;
        m_single_view_cache.clear();
    }

    void append(std::string_view data) {
        if (data.empty()) {
            return;
        }
        m_total_bytes += data.size();
        if (m_chunk_count == 0) {
            m_single_chunk.assign(data.data(), data.size());
            m_chunk_count = 1;
            m_single_view_cache.clear();
            return;
        }

        ensureChunkVector();
        m_chunks.emplace_back(data);
        ++m_chunk_count;
        m_single_view_cache.clear();
    }

    void append(std::string&& data) {
        if (data.empty()) {
            return;
        }
        m_total_bytes += data.size();
        if (m_chunk_count == 0) {
            m_single_chunk = std::move(data);
            m_chunk_count = 1;
            m_single_view_cache.clear();
            return;
        }

        ensureChunkVector();
        m_chunks.push_back(std::move(data));
        ++m_chunk_count;
        m_single_view_cache.clear();
    }

    const std::vector<std::string>& view() const {
        if (m_chunk_count == 0 || !m_chunks.empty()) {
            return m_chunks;
        }

        m_single_view_cache.clear();
        m_single_view_cache.push_back(m_single_chunk);
        return m_single_view_cache;
    }

    std::vector<std::string> takeChunks() {
        if (m_chunk_count == 0) {
            return {};
        }

        m_single_view_cache.clear();
        m_total_bytes = 0;
        m_chunk_count = 0;

        if (!m_chunks.empty()) {
            m_single_chunk.clear();
            return std::exchange(m_chunks, {});
        }

        std::vector<std::string> out;
        out.reserve(1);
        out.push_back(std::move(m_single_chunk));
        m_single_chunk.clear();
        return out;
    }

    std::string coalesce() const {
        if (m_chunk_count == 0) {
            return {};
        }

        if (m_chunks.empty()) {
            return m_single_chunk;
        }

        std::string out;
        out.reserve(m_total_bytes);
        for (const auto& chunk : m_chunks) {
            out.append(chunk);
        }
        return out;
    }

    std::string takeCoalesced() {
        if (m_chunk_count == 0) {
            return {};
        }

        if (m_chunks.empty()) {
            m_total_bytes = 0;
            m_chunk_count = 0;
            m_single_view_cache.clear();
            return std::exchange(m_single_chunk, {});
        }

        auto out = coalesce();
        clear();
        return out;
    }

    std::string takeSingleChunk() {
        if (m_chunk_count == 1 && m_chunks.empty()) {
            m_total_bytes = 0;
            m_chunk_count = 0;
            m_single_view_cache.clear();
            return std::exchange(m_single_chunk, {});
        }
        return takeCoalesced();
    }

    explicit operator std::string() const {
        return coalesce();
    }

private:
    void ensureChunkVector() {
        if (m_chunk_count == 1 && m_chunks.empty()) {
            m_chunks.reserve(2);
            m_chunks.push_back(std::move(m_single_chunk));
        }
    }

    std::string m_single_chunk;
    std::vector<std::string> m_chunks;
    size_t m_total_bytes = 0;
    size_t m_chunk_count = 0;
    mutable std::vector<std::string> m_single_view_cache;
};

namespace detail {

enum class Http2RequestCommonHeaderIndex : uint8_t {
    ContentLength = 0,
    ContentType,
    AcceptEncoding,
    UserAgent,
    Count
};

inline bool equalsLowerAscii(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(lhs[i]);
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch - 'A' + 'a');
        }
        if (ch != static_cast<unsigned char>(rhs[i])) {
            return false;
        }
    }
    return true;
}

inline std::optional<Http2RequestCommonHeaderIndex>
matchHttp2RequestCommonHeader(std::string_view name) {
    if (equalsLowerAscii(name, "content-length")) {
        return Http2RequestCommonHeaderIndex::ContentLength;
    }
    if (equalsLowerAscii(name, "content-type")) {
        return Http2RequestCommonHeaderIndex::ContentType;
    }
    if (equalsLowerAscii(name, "accept-encoding")) {
        return Http2RequestCommonHeaderIndex::AcceptEncoding;
    }
    if (equalsLowerAscii(name, "user-agent")) {
        return Http2RequestCommonHeaderIndex::UserAgent;
    }
    return std::nullopt;
}

} // namespace detail

/**
 * @brief HTTP/2 静态响应配置。
 * @details 该结构描述 exact path 命中的预配置响应。body 由配置对象持有；
 *          static fast path 会复用预编码 header block，避免热路径重复 HPACK 编码。
 */
struct H2StaticResponse
{
    int status = 200;                                      ///< HTTP status code，默认 200。
    std::string content_type = "application/octet-stream"; ///< Content-Type，默认二进制。
    std::string body;                                      ///< 静态响应 body，允许为空。
    bool allow_head = true;                                ///< 是否允许 HEAD 复用该静态响应。
};

/**
 * @brief HTTP/2 exact path 静态路由配置。
 * @details path 按原始 `:path` 精确匹配；encoded_headers 由 builder/runtime config
 *          预先填充，运行时只读复用。
 */
struct H2StaticRoute
{
    std::string path;                                      ///< 精确匹配的 request `:path`。
    H2StaticResponse response;                             ///< 命中后发送的静态响应。
    std::shared_ptr<const std::string> encoded_headers;     ///< 预编码 HPACK 响应头块。
    std::shared_ptr<const std::string> shared_body;         ///< 预持有响应体，供 DATA frame 快路径复用。
};

inline std::shared_ptr<const std::string> encodeH2StaticResponseHeaders(
    const H2StaticResponse& response) {
    HpackEncoder encoder;
    auto block = encoder.encodeStateless({
        {":status", std::to_string(response.status)},
        {"content-type", response.content_type},
        {"content-length", std::to_string(response.body.size())},
    });
    return std::make_shared<const std::string>(std::move(block));
}

inline void prepareH2StaticRoute(H2StaticRoute& route) {
    if (!route.encoded_headers) {
        route.encoded_headers = encodeH2StaticResponseHeaders(route.response);
    }
    if (!route.response.body.empty() && !route.shared_body) {
        route.shared_body = std::make_shared<const std::string>(route.response.body);
    }
}

inline H2StaticRoute makeH2StaticRoute(std::string path, H2StaticResponse response) {
    H2StaticRoute route{
        .path = std::move(path),
        .response = std::move(response),
    };
    prepareH2StaticRoute(route);
    return route;
}

/**
 * @brief HTTP/2 请求结构
 */
struct Http2Request
{
    std::string method;
    std::string scheme;
    std::string authority;
    std::string path;
    std::vector<Http2HeaderField> headers;
    Http2ChunkedBody body;
    
    // 获取伪头部
    std::string getHeader(const std::string& name) const {
        if (const auto* common = getCommonHeaderPtr(name); common != nullptr) {
            return *common;
        }
        for (const auto& h : headers) {
            if (h.name == name) return h.value;
        }
        return "";
    }

    void setCommonHeader(detail::Http2RequestCommonHeaderIndex idx, std::string value) {
        const size_t index = static_cast<size_t>(idx);
        m_common_headers[index] = std::move(value);
        m_common_header_present.set(index);
    }

    void setBody(std::string data) { body.set(std::move(data)); }
    void clear() {
        method.clear();
        scheme.clear();
        authority.clear();
        path.clear();
        headers.clear();
        body.clear();
        for (auto& value : m_common_headers) {
            value.clear();
        }
        m_common_header_present.reset();
    }
    size_t bodySize() const { return body.size(); }
    size_t bodyChunkCount() const { return body.chunkCount(); }
    const std::vector<std::string>& bodyChunks() const { return body.view(); }
    std::vector<std::string> takeBodyChunks() { return body.takeChunks(); }
    std::string coalescedBody() const { return body.coalesce(); }
    std::string takeCoalescedBody() { return body.takeCoalesced(); }
    std::string takeSingleBodyChunk() { return body.takeSingleChunk(); }

private:
    const std::string* getCommonHeaderPtr(std::string_view name) const {
        auto slot = detail::matchHttp2RequestCommonHeader(name);
        if (!slot) {
            return nullptr;
        }

        const size_t index = static_cast<size_t>(*slot);
        if (!m_common_header_present.test(index)) {
            return nullptr;
        }
        return &m_common_headers[index];
    }

    std::array<std::string, static_cast<size_t>(detail::Http2RequestCommonHeaderIndex::Count)> m_common_headers{};
    std::bitset<static_cast<size_t>(detail::Http2RequestCommonHeaderIndex::Count)> m_common_header_present;
};

/**
 * @brief HTTP/2 响应结构
 */
struct Http2Response
{
    int status = 200;
    std::vector<Http2HeaderField> headers;
    std::string body;
    
    void setHeader(const std::string& name, const std::string& value) {
        for (auto& h : headers) {
            if (h.name == name) {
                h.value = value;
                return;
            }
        }
        headers.push_back({name, value});
    }
    
    void setStatus(int code) { status = code; }
    void setBody(std::string data) { body = std::move(data); }
    void clear() {
        status = 200;
        headers.clear();
        body.clear();
    }
};

/**
 * @brief HTTP/2 流
 */
enum class Http2StreamEvent : uint32_t {
    None = 0,
    HeadersReady = 1u << 0,
    DataArrived = 1u << 1,
    RequestComplete = 1u << 2,
    ResponseComplete = 1u << 3,
    Reset = 1u << 4,
    WindowUpdated = 1u << 5,
};

constexpr Http2StreamEvent operator|(Http2StreamEvent lhs, Http2StreamEvent rhs) noexcept {
    using U = std::underlying_type_t<Http2StreamEvent>;
    return static_cast<Http2StreamEvent>(static_cast<U>(lhs) | static_cast<U>(rhs));
}

constexpr Http2StreamEvent operator&(Http2StreamEvent lhs, Http2StreamEvent rhs) noexcept {
    using U = std::underlying_type_t<Http2StreamEvent>;
    return static_cast<Http2StreamEvent>(static_cast<U>(lhs) & static_cast<U>(rhs));
}

inline Http2StreamEvent& operator|=(Http2StreamEvent& lhs, Http2StreamEvent rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool hasHttp2StreamEvent(Http2StreamEvent events, Http2StreamEvent event) noexcept {
    using U = std::underlying_type_t<Http2StreamEvent>;
    return (static_cast<U>(events & event) != 0);
}

class Http2Stream : public std::enable_shared_from_this<Http2Stream>
{
public:
    using ptr = std::shared_ptr<Http2Stream>;

    class ReplyAndWaitAwaitable
        : public galay::kernel::TimeoutSupport<ReplyAndWaitAwaitable>
    {
    public:
        explicit ReplyAndWaitAwaitable(Http2OutgoingFrame::WaiterPtr waiter)
            : m_waiter(std::move(waiter))
            , m_wait_awaitable(m_waiter.get())
        {
        }

        bool await_ready() const noexcept {
            return m_wait_awaitable.await_ready();
        }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            return m_wait_awaitable.await_suspend(handle);
        }

        ReplyAndWaitAwaitable& wait() & { return *this; }
        ReplyAndWaitAwaitable&& wait() && { return std::move(*this); }

        std::expected<void, galay::kernel::IOError> await_resume() {
            return m_wait_awaitable.await_resume();
        }

    private:
        Http2OutgoingFrame::WaiterPtr m_waiter;
        galay::kernel::AsyncWaiterAwaitable<void> m_wait_awaitable;
    };

    class CompletionAwaitable
        : public galay::kernel::TimeoutSupport<CompletionAwaitable>
    {
    public:
        CompletionAwaitable(std::shared_ptr<Http2Stream> keepalive,
                            galay::kernel::AsyncWaiter<void>* waiter)
            : m_keepalive(std::move(keepalive))
            , m_wait_awaitable(waiter)
        {
        }

        bool await_ready() const noexcept {
            return m_wait_awaitable.await_ready();
        }

        template<typename Handle>
        bool await_suspend(Handle handle) noexcept {
            return m_wait_awaitable.await_suspend(handle);
        }

        std::expected<void, galay::kernel::IOError> await_resume() {
            return m_wait_awaitable.await_resume();
        }

    private:
        std::shared_ptr<Http2Stream> m_keepalive;
        galay::kernel::AsyncWaiterAwaitable<void> m_wait_awaitable;
    };

    // 流 ID
    uint32_t streamId() const { return m_stream_id; }
    
    // 流状态
    Http2StreamState state() const { return m_state; }
    void setState(Http2StreamState state) { m_state = state; }
    
    // 流量控制窗口
    int32_t sendWindow() const { return m_send_window; }
    int32_t recvWindow() const { return m_recv_window; }
    
    void adjustSendWindow(int32_t delta) { m_send_window += delta; }
    void adjustRecvWindow(int32_t delta) { m_recv_window += delta; }
    
    // END_STREAM 标志
    bool isEndStreamReceived() const { return m_end_stream_received; }
    bool isEndStreamSent() const { return m_end_stream_sent; }
    void setEndStreamReceived() { m_end_stream_received = true; }
    void setEndStreamSent() { m_end_stream_sent = true; }
    
    // END_HEADERS 标志
    bool isEndHeadersReceived() const { return m_end_headers_received; }
    void setEndHeadersReceived() { m_end_headers_received = true; }
    
    // 头部块累积（用于 CONTINUATION）
    void appendHeaderBlock(const std::string& data) { m_header_block.append(data); }
    void appendHeaderBlock(std::string_view data) { m_header_block.append(data.data(), data.size()); }
    const std::string& headerBlock() const { return m_header_block; }
    void clearHeaderBlock() { m_header_block.clear(); }

    // 已解码的头部字段（由 StreamManager 在 readerLoop 中按帧顺序解码）
    void setDecodedHeaders(std::vector<Http2HeaderField> fields) { m_decoded_headers = std::move(fields); }
    const std::vector<Http2HeaderField>& decodedHeaders() const { return m_decoded_headers; }
    bool hasDecodedHeaders() const { return !m_decoded_headers.empty(); }
    void clearDecodedHeaders() { m_decoded_headers.clear(); }
    
    // 请求/响应数据
    Http2Request& request() { return m_request; }
    const Http2Request& request() const { return m_request; }
    
    Http2Response& response() { return m_response; }
    const Http2Response& response() const { return m_response; }
    
    // 数据累积
    void appendData(const std::string& data) { m_request.body.append(data); }
    void appendData(std::string&& data) { m_request.body.append(std::move(data)); }
    
    // 状态转换
    bool canReceiveHeaders() const {
        return m_state == Http2StreamState::Idle || 
               m_state == Http2StreamState::ReservedRemote ||
               m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedLocal;
    }
    
    bool canReceiveData() const {
        return m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedLocal;
    }
    
    bool canSendHeaders() const {
        return m_state == Http2StreamState::Idle ||
               m_state == Http2StreamState::ReservedLocal ||
               m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedRemote;
    }
    
    bool canSendData() const {
        return m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedRemote;
    }
    
    // 处理接收到的帧后的状态转换
    void onHeadersReceived(bool end_stream) {
        if (m_state == Http2StreamState::Idle) {
            m_state = Http2StreamState::Open;
        }
        if (end_stream) {
            m_end_stream_received = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedRemote;
            } else if (m_state == Http2StreamState::HalfClosedLocal) {
                m_state = Http2StreamState::Closed;
            }
        }
        notifyRetireIfClosed();
    }
    
    void onDataReceived(bool end_stream) {
        if (end_stream) {
            m_end_stream_received = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedRemote;
            } else if (m_state == Http2StreamState::HalfClosedLocal) {
                m_state = Http2StreamState::Closed;
            }
        }
        notifyRetireIfClosed();
    }
    
    // 处理发送帧后的状态转换
    void onHeadersSent(bool end_stream) {
        if (m_state == Http2StreamState::Idle) {
            m_state = Http2StreamState::Open;
        } else if (m_state == Http2StreamState::ReservedLocal) {
            m_state = Http2StreamState::HalfClosedRemote;
        }
        if (end_stream) {
            m_end_stream_sent = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedLocal;
            } else if (m_state == Http2StreamState::HalfClosedRemote) {
                m_state = Http2StreamState::Closed;
            }
        }
        notifyRetireIfClosed();
    }
    
    void onDataSent(bool end_stream) {
        if (end_stream) {
            m_end_stream_sent = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedLocal;
            } else if (m_state == Http2StreamState::HalfClosedRemote) {
                m_state = Http2StreamState::Closed;
            }
        }
        notifyRetireIfClosed();
    }
    
    void onRstStreamReceived() {
        m_state = Http2StreamState::Closed;
        notifyRetireIfClosed();
    }
    
    void onRstStreamSent() {
        m_state = Http2StreamState::Closed;
        notifyRetireIfClosed();
    }

private:
    struct PendingDataSend {
        std::shared_ptr<const std::string> payload;
        size_t offset = 0;
        bool end_stream = false;
        Http2OutgoingFrame::WaiterPtr waiter;
    };

    static uint32_t normalizeMaxFrameSize(uint32_t max_frame_size) {
        if (max_frame_size < kDefaultMaxFrameSize) {
            return kDefaultMaxFrameSize;
        }
        return std::min<uint32_t>(max_frame_size, kMaxFrameSize);
    }

    int32_t availableConnSendWindow() const {
        return m_conn_send_window != nullptr
            ? *m_conn_send_window
            : std::numeric_limits<int32_t>::max();
    }

    void consumeDataSendWindow(size_t bytes) {
        const auto delta = static_cast<int32_t>(bytes);
        m_send_window -= delta;
        if (m_conn_send_window != nullptr) {
            *m_conn_send_window -= delta;
        }
    }

    size_t availableDataChunkSize(const PendingDataSend& pending) const {
        if (!pending.payload || pending.offset >= pending.payload->size()) {
            return 0;
        }
        const auto stream_window = std::max<int32_t>(m_send_window, 0);
        const auto conn_window = std::max<int32_t>(availableConnSendWindow(), 0);
        if (stream_window == 0 || conn_window == 0) {
            return 0;
        }
        return std::min<size_t>({
            pending.payload->size() - pending.offset,
            static_cast<size_t>(m_max_frame_size),
            static_cast<size_t>(stream_window),
            static_cast<size_t>(conn_window)
        });
    }

    void enqueueOutgoingDataFrame(Http2OutgoingFrame&& frame) {
        if (m_send_channel) {
            m_send_channel->send(std::move(frame));
            return;
        }
        if (m_send_queue) {
            m_send_queue->push_back(std::move(frame));
        }
    }

    enum class PendingDataFlushResult {
        Blocked,
        Progress,
        Complete
    };

    PendingDataFlushResult flushPendingDataFront(PendingDataSend& pending) {
        const size_t payload_size = pending.payload ? pending.payload->size() : 0;
        if (payload_size == 0) {
            auto header_bytes = Http2FrameBuilder::dataHeaderBytes(
                m_stream_id, 0, pending.end_stream);
            if (pending.end_stream) {
                onDataSent(true);
            }
            auto payload = pending.payload
                ? pending.payload
                : std::make_shared<const std::string>();
            enqueueOutgoingDataFrame(
                Http2OutgoingFrame::segmentedShared(
                    std::move(header_bytes), std::move(payload), pending.waiter));
            return PendingDataFlushResult::Complete;
        }

        const size_t chunk_size = availableDataChunkSize(pending);
        if (chunk_size == 0) {
            return PendingDataFlushResult::Blocked;
        }

        const bool final_chunk = pending.offset + chunk_size == payload_size;
        const bool frame_end_stream = pending.end_stream && final_chunk;
        auto header_bytes = Http2FrameBuilder::dataHeaderBytes(
            m_stream_id, chunk_size, frame_end_stream);
        auto waiter = final_chunk ? pending.waiter : nullptr;
        enqueueOutgoingDataFrame(
            Http2OutgoingFrame::segmentedShared(
                std::move(header_bytes),
                pending.payload,
                pending.offset,
                chunk_size,
                std::move(waiter)));

        consumeDataSendWindow(chunk_size);
        pending.offset += chunk_size;
        if (frame_end_stream) {
            onDataSent(true);
        }
        return final_chunk ? PendingDataFlushResult::Complete
                           : PendingDataFlushResult::Progress;
    }

    bool flushPendingData() {
        bool made_progress = false;
        while (!m_pending_data.empty() && (m_send_queue || m_send_channel)) {
            auto result = flushPendingDataFront(m_pending_data.front());
            if (result == PendingDataFlushResult::Blocked) {
                break;
            }
            made_progress = true;
            if (result == PendingDataFlushResult::Complete) {
                m_pending_data.pop_front();
            }
        }
        return made_progress;
    }

    void notifyPendingDataWaiters() {
        for (auto& pending : m_pending_data) {
            if (pending.waiter) {
                pending.waiter->notify();
            }
        }
        m_pending_data.clear();
    }

    void queueDataForSend(std::shared_ptr<const std::string> payload,
                          bool end_stream,
                          const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        m_pending_data.push_back(PendingDataSend{
            .payload = payload ? std::move(payload) : std::make_shared<const std::string>(),
            .offset = 0,
            .end_stream = end_stream,
            .waiter = waiter
        });
        flushPendingData();
    }

public:
    // ==================== 帧队列 ====================

    /**
     * @brief 推入帧（由 StreamManager 调用）
     */
    void pushFrame(Http2Frame::uptr frame) {
        if (!m_frame_queue_enabled) {
            return;
        }
        m_frame_channel.send(std::move(frame));
    }

    /**
     * @brief 标记帧队列关闭（由 StreamManager 在 RST_STREAM/GOAWAY 时调用）
     */
    void closeFrameQueue() {
        if (m_frame_queue_closed) return;
        m_frame_queue_closed = true;
        notifyPendingDataWaiters();
        markRequestCompleted();
        markResponseCompleted();
        m_frame_channel.send(Http2Frame::uptr{});
    }

    bool isFrameQueueClosed() const { return m_frame_queue_closed; }

    void setFrameQueueEnabled(bool enabled) { m_frame_queue_enabled = enabled; }
    bool isFrameQueueEnabled() const { return m_frame_queue_enabled; }

    void setGoAwayError(Http2GoAwayError error) { m_goaway_error = std::move(error); }
    bool hasGoAwayError() const { return m_goaway_error.has_value(); }
    const std::optional<Http2GoAwayError>& goAwayError() const { return m_goaway_error; }

    Http2StreamEvent takeEvents() {
        auto events = m_pending_events;
        m_pending_events = Http2StreamEvent::None;
        m_active_queued = false;
        return events;
    }

    /**
     * @brief 获取下一帧的 Awaitable
     * @return co_await 后得到 expected<uptr, IOError>，空指针表示流已关闭
     */
    auto getFrame() {
        return m_frame_channel.recv();
    }

    /**
     * @brief 批量获取帧（至少 1 帧，最多 max_count）
     * @return co_await 后得到 expected<vector<uptr>, IOError>
     */
    auto getFrames(size_t max_count = galay::kernel::UnsafeChannel<Http2Frame::uptr>::DEFAULT_BATCH_SIZE) {
        return m_frame_channel.recvBatch(max_count);
    }

    /**
     * @brief 解码头部块
     */
    std::vector<Http2HeaderField> decodeHeaders(const std::string& header_block) {
        if (!m_decoder) return {};
        auto result = m_decoder->decode(header_block);
        if (!result) return {};
        return std::move(result.value());
    }

    void consumeDecodedHeadersAsRequest() {
        if (!m_decoded_headers.empty()) {
            m_request.headers.reserve(m_request.headers.size() + m_decoded_headers.size());
        }
        for (auto& f : m_decoded_headers) {
            if (f.name == ":method") m_request.method = std::move(f.value);
            else if (f.name == ":scheme") m_request.scheme = std::move(f.value);
            else if (f.name == ":authority") m_request.authority = std::move(f.value);
            else if (f.name == ":path") m_request.path = std::move(f.value);
            else if (f.name == "content-length") {
                m_request.setCommonHeader(detail::Http2RequestCommonHeaderIndex::ContentLength,
                                          std::move(f.value));
            }
            else if (f.name == "content-type") {
                m_request.setCommonHeader(detail::Http2RequestCommonHeaderIndex::ContentType,
                                          std::move(f.value));
            }
            else if (f.name == "accept-encoding") {
                m_request.setCommonHeader(detail::Http2RequestCommonHeaderIndex::AcceptEncoding,
                                          std::move(f.value));
            }
            else if (f.name == "user-agent") {
                m_request.setCommonHeader(detail::Http2RequestCommonHeaderIndex::UserAgent,
                                          std::move(f.value));
            }
            else m_request.headers.push_back(std::move(f));
        }
        clearDecodedHeaders();
    }

    void consumeDecodedHeadersAsResponse() {
        if (!m_decoded_headers.empty()) {
            m_response.headers.reserve(m_response.headers.size() + m_decoded_headers.size());
        }
        for (auto& f : m_decoded_headers) {
            if (f.name == ":status") {
                int status = 0;
                std::from_chars(f.value.data(), f.value.data() + f.value.size(), status);
                m_response.status = status;
            } else {
                m_response.headers.push_back(std::move(f));
            }
        }
        clearDecodedHeaders();
    }

    void appendRequestData(const std::string& data) {
        m_request.body.append(data);
    }

    void appendRequestData(std::string&& data) {
        m_request.body.append(std::move(data));
    }

    void appendRequestData(std::string_view data) {
        m_request.body.append(data);
    }

    void appendResponseData(const std::string& data) {
        m_response.body.append(data);
    }

    void markRequestCompleted() {
        if (m_request_completed) {
            return;
        }
        m_request_completed = true;
        m_request_waiter.notify();
    }

    void markResponseCompleted() {
        if (m_response_completed) {
            return;
        }
        m_response_completed = true;
        m_response_waiter.notify();
    }

    bool isRequestCompleted() const {
        return m_request_completed;
    }

    bool isResponseCompleted() const {
        return m_response_completed;
    }

    CompletionAwaitable waitRequestComplete() {
        return CompletionAwaitable(shared_from_this(), &m_request_waiter);
    }

    CompletionAwaitable waitResponseComplete() {
        return CompletionAwaitable(shared_from_this(), &m_response_waiter);
    }

    // ==================== 发送接口 ====================

    /**
     * @brief 发送 HEADERS 帧
     */
    void sendHeaders(const std::vector<Http2HeaderField>& headers,
                     bool end_stream = false, bool end_headers = true) {
        sendHeadersInternal(headers, end_stream, end_headers, nullptr);
    }

    void sendEncodedHeaders(const std::string& header_block,
                            bool end_stream = false,
                            bool end_headers = true) {
        sendEncodedHeadersInternal(header_block, end_stream, end_headers, nullptr);
    }

    void sendEncodedHeaders(std::string&& header_block,
                            bool end_stream = false,
                            bool end_headers = true) {
        sendEncodedHeadersInternal(std::move(header_block), end_stream, end_headers, nullptr);
    }

    void sendEncodedHeaders(std::shared_ptr<const std::string> header_block,
                            bool end_stream = false,
                            bool end_headers = true) {
        sendEncodedHeadersInternal(std::move(header_block), end_stream, end_headers, nullptr);
    }

    /**
     * @brief 发送 DATA 帧
     */
    void sendData(const std::string& data, bool end_stream = false) {
        sendDataInternal(data, end_stream, nullptr);
    }

    void sendData(std::string&& data, bool end_stream = false) {
        sendDataInternal(std::move(data), end_stream, nullptr);
    }

    void sendData(std::shared_ptr<const std::string> data, bool end_stream = false) {
        sendDataInternal(std::move(data), end_stream, nullptr);
    }

    /**
     * @brief 发送 RST_STREAM 帧
     */
    void sendRstStream(Http2ErrorCode error) {
        sendRstStreamInternal(error, nullptr);
    }

    /**
     * @brief 批量发送帧（按顺序入队）
     */
    void sendFrames(std::vector<Http2Frame::uptr> frames) {
        sendFrameBatchInternal(std::move(frames), nullptr);
    }

    /**
     * @brief 批量发送 DATA 帧（最后一帧可带 END_STREAM）
     */
    void sendDataBatch(const std::vector<std::string>& chunks, bool end_stream = false) {
        sendDataBatchInternal(chunks, end_stream, nullptr);
    }

    void sendDataChunks(std::vector<std::string>&& chunks, bool end_stream = false) {
        sendDataChunksInternal(std::move(chunks), end_stream, nullptr);
    }

    void sendHeadersAndData(const std::vector<Http2HeaderField>& headers,
                            std::string data,
                            bool end_headers = true) {
        sendHeadersAndDataInternal(headers, std::move(data), end_headers, nullptr);
    }

    void sendHeadersAndDataChunks(const std::vector<Http2HeaderField>& headers,
                                  std::vector<std::string>&& chunks,
                                  bool end_headers = true) {
        sendHeadersAndDataChunksInternal(headers, std::move(chunks), end_headers, nullptr);
    }

    void sendEncodedHeadersAndData(std::string header_block,
                                   std::string data,
                                   bool end_headers = true) {
        sendEncodedHeadersAndDataInternal(
            std::move(header_block), std::move(data), end_headers, nullptr);
    }

    void sendEncodedHeadersAndData(std::shared_ptr<const std::string> header_block,
                                   std::string data,
                                   bool end_headers = true) {
        sendEncodedHeadersAndDataInternal(
            std::move(header_block), std::move(data), end_headers, nullptr);
    }

    void sendEncodedHeadersAndDataChunks(std::string header_block,
                                         std::vector<std::string>&& chunks,
                                         bool end_headers = true) {
        sendEncodedHeadersAndDataChunksInternal(
            std::move(header_block), std::move(chunks), end_headers, nullptr);
    }

    /**
     * @brief 帧优先 API：发送 HEADERS 并等待入队完成
     */
    ReplyAndWaitAwaitable replyHeader(const std::vector<Http2HeaderField>& headers,
                                      bool end_stream = false,
                                      bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendHeadersInternal(headers, end_stream, end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyEncodedHeaders(const std::string& header_block,
                                             bool end_stream = false,
                                             bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendEncodedHeadersInternal(header_block, end_stream, end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyEncodedHeaders(std::string&& header_block,
                                             bool end_stream = false,
                                             bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendEncodedHeadersInternal(std::move(header_block), end_stream, end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyEncodedHeaders(std::shared_ptr<const std::string> header_block,
                                             bool end_stream = false,
                                             bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendEncodedHeadersInternal(std::move(header_block), end_stream, end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 帧优先 API：发送 DATA 并等待入队完成
     */
    ReplyAndWaitAwaitable replyData(const std::string& data, bool end_stream = false) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendDataInternal(data, end_stream, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyData(std::string&& data, bool end_stream = false) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendDataInternal(std::move(data), end_stream, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyData(std::shared_ptr<const std::string> data, bool end_stream = false) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendDataInternal(std::move(data), end_stream, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 帧优先 API：发送 RST_STREAM 并等待入队完成
     */
    ReplyAndWaitAwaitable replyRst(Http2ErrorCode error) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendRstStreamInternal(error, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 帧优先 API：批量发送帧并等待“最后一帧入队”完成
     */
    ReplyAndWaitAwaitable replyFrames(std::vector<Http2Frame::uptr> frames) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendFrameBatchInternal(std::move(frames), waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 帧优先 API：批量发送 DATA 并等待最后一帧入队
     */
    ReplyAndWaitAwaitable replyDataBatch(const std::vector<std::string>& chunks,
                                         bool end_stream = false) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendDataBatchInternal(chunks, end_stream, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyDataChunks(std::vector<std::string>&& chunks,
                                          bool end_stream = false) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendDataChunksInternal(std::move(chunks), end_stream, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyHeadersAndData(const std::vector<Http2HeaderField>& headers,
                                             std::string data,
                                             bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendHeadersAndDataInternal(headers, std::move(data), end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyHeadersAndDataChunks(const std::vector<Http2HeaderField>& headers,
                                                    std::vector<std::string>&& chunks,
                                                    bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendHeadersAndDataChunksInternal(headers, std::move(chunks), end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyEncodedHeadersAndData(std::string header_block,
                                                     std::string data,
                                                     bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendEncodedHeadersAndDataInternal(
            std::move(header_block), std::move(data), end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyEncodedHeadersAndData(std::shared_ptr<const std::string> header_block,
                                                     std::string data,
                                                     bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendEncodedHeadersAndDataInternal(
            std::move(header_block), std::move(data), end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    ReplyAndWaitAwaitable replyEncodedHeadersAndDataChunks(std::string header_block,
                                                           std::vector<std::string>&& chunks,
                                                           bool end_headers = true) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        sendEncodedHeadersAndDataChunksInternal(
            std::move(header_block), std::move(chunks), end_headers, waiter);
        return ReplyAndWaitAwaitable(std::move(waiter));
    }

    /**
     * @brief 发送 WINDOW_UPDATE 帧
     */
    void sendWindowUpdate(uint32_t increment) {
        if (!m_send_queue && !m_send_channel) return;

        auto frame = std::make_unique<Http2WindowUpdateFrame>();
        frame->header().stream_id = m_stream_id;
        frame->setWindowSizeIncrement(increment);

        if (m_send_channel) {
            m_send_channel->send(Http2OutgoingFrame{std::move(frame)});
        } else {
            m_send_queue->push_back(Http2OutgoingFrame{std::move(frame)});
        }
    }

public:
    // ==================== 优先级 ====================

    uint8_t weight() const { return m_weight; }
    uint32_t streamDependency() const { return m_stream_dependency; }
    bool exclusive() const { return m_exclusive; }

    void setPriority(bool exclusive, uint32_t dependency, uint8_t weight) {
        m_exclusive = exclusive;
        m_stream_dependency = dependency;
        m_weight = weight;
    }

private:
    explicit Http2Stream(uint32_t stream_id)
        : m_stream_id(stream_id)
        , m_state(Http2StreamState::Idle)
        , m_send_window(kDefaultInitialWindowSize)
        , m_recv_window(kDefaultInitialWindowSize)
        , m_end_stream_received(false)
        , m_end_stream_sent(false)
        , m_end_headers_received(false)
    {
    }

    static ptr create(uint32_t stream_id) {
        return ptr(new Http2Stream(stream_id));
    }

    void attachIO(std::vector<Http2OutgoingFrame>* send_queue,
                  HpackEncoder* encoder,
                  HpackDecoder* decoder,
                  int32_t* conn_send_window = nullptr,
                  uint32_t max_frame_size = kDefaultMaxFrameSize) {
        m_send_queue = send_queue;
        m_send_channel = nullptr;
        m_encoder = encoder;
        m_decoder = decoder;
        m_conn_send_window = conn_send_window;
        m_max_frame_size = normalizeMaxFrameSize(max_frame_size);
        m_io_attached = true;
        flushPendingData();
    }

    void attachIO(galay::kernel::MpscChannel<Http2OutgoingFrame>* send_channel,
                  HpackEncoder* encoder,
                  HpackDecoder* decoder,
                  int32_t* conn_send_window = nullptr,
                  uint32_t max_frame_size = kDefaultMaxFrameSize) {
        m_send_channel = send_channel;
        m_send_queue = nullptr;
        m_encoder = encoder;
        m_decoder = decoder;
        m_conn_send_window = conn_send_window;
        m_max_frame_size = normalizeMaxFrameSize(max_frame_size);
        m_io_attached = true;
        flushPendingData();
    }

    void setRetireCallback(std::function<void(uint32_t)> callback) {
        m_retire_callback = std::move(callback);
    }

    uint32_t m_stream_id;
    Http2StreamState m_state;
    int32_t m_send_window;
    int32_t m_recv_window;
    bool m_end_stream_received;
    bool m_end_stream_sent;
    bool m_end_headers_received;
    std::string m_header_block;
    std::vector<Http2HeaderField> m_decoded_headers;
    Http2Request m_request;
    Http2Response m_response;

    // 帧通道
    galay::kernel::UnsafeChannel<Http2Frame::uptr> m_frame_channel;
    bool m_frame_queue_closed = false;
    bool m_frame_queue_enabled = true;
    std::optional<Http2GoAwayError> m_goaway_error;
    Http2StreamEvent m_pending_events = Http2StreamEvent::None;
    bool m_active_queued = false;
    galay::kernel::AsyncWaiter<void> m_request_waiter;
    galay::kernel::AsyncWaiter<void> m_response_waiter;
    bool m_request_completed = false;
    bool m_response_completed = false;

    // 优先级
    uint8_t m_weight = 16;
    uint32_t m_stream_dependency = 0;
    bool m_exclusive = false;

    // 发送队列和编解码器（由 StreamManager 绑定）
    galay::kernel::MpscChannel<Http2OutgoingFrame>* m_send_channel = nullptr;
    std::vector<Http2OutgoingFrame>* m_send_queue = nullptr;
    HpackEncoder* m_encoder = nullptr;
    HpackDecoder* m_decoder = nullptr;
    int32_t* m_conn_send_window = nullptr;
    uint32_t m_max_frame_size = kDefaultMaxFrameSize;
    std::deque<PendingDataSend> m_pending_data;
    bool m_io_attached = false;
    std::function<void(uint32_t)> m_retire_callback;

    template<typename SocketType>
    friend class Http2StreamManagerImpl;
    template<typename SocketType>
    friend class Http2ConnImpl;
    friend class Http2StreamPool;
    friend class Http2ActiveStreamBatch;

    void notifyRetireIfClosed() {
        if (m_state == Http2StreamState::Closed && m_retire_callback) {
            m_retire_callback(m_stream_id);
        }
    }

    void resetForReuse(uint32_t stream_id) {
        m_stream_id = stream_id;
        m_state = Http2StreamState::Idle;
        m_send_window = kDefaultInitialWindowSize;
        m_recv_window = kDefaultInitialWindowSize;
        m_end_stream_received = false;
        m_end_stream_sent = false;
        m_end_headers_received = false;
        m_header_block.clear();
        m_decoded_headers.clear();
        m_request.clear();
        m_response.clear();

        std::destroy_at(&m_frame_channel);
        std::construct_at(&m_frame_channel);
        m_frame_queue_closed = false;
        m_frame_queue_enabled = true;
        m_goaway_error.reset();
        m_pending_events = Http2StreamEvent::None;
        m_active_queued = false;

        std::destroy_at(&m_request_waiter);
        std::construct_at(&m_request_waiter);
        std::destroy_at(&m_response_waiter);
        std::construct_at(&m_response_waiter);
        m_request_completed = false;
        m_response_completed = false;

        m_weight = 16;
        m_stream_dependency = 0;
        m_exclusive = false;

        m_send_channel = nullptr;
        m_send_queue = nullptr;
        m_encoder = nullptr;
        m_decoder = nullptr;
        m_conn_send_window = nullptr;
        m_max_frame_size = kDefaultMaxFrameSize;
        m_pending_data.clear();
        m_io_attached = false;
        m_retire_callback = nullptr;
    }

    void sendHeadersInternal(const std::vector<Http2HeaderField>& headers,
                             bool end_stream,
                             bool end_headers,
                             const Http2OutgoingFrame::WaiterPtr& waiter) {
        if ((!m_send_queue && !m_send_channel) || !m_encoder) return;

        std::string header_block = m_encoder->encode(headers);
        auto header_bytes = Http2FrameBuilder::headersHeaderBytes(m_stream_id,
                                                                  header_block.size(),
                                                                  end_stream,
                                                                  end_headers);

        onHeadersSent(end_stream);
        if (m_send_channel) {
            m_send_channel->send(
                Http2OutgoingFrame::segmented(std::move(header_bytes), std::move(header_block), waiter));
        } else {
            m_send_queue->push_back(
                Http2OutgoingFrame::segmented(std::move(header_bytes), std::move(header_block), waiter));
        }
    }

    void sendEncodedHeadersInternal(const std::string& header_block,
                                    bool end_stream,
                                    bool end_headers,
                                    const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) return;

        auto header_bytes = Http2FrameBuilder::headersHeaderBytes(m_stream_id,
                                                                  header_block.size(),
                                                                  end_stream,
                                                                  end_headers);

        onHeadersSent(end_stream);
        if (m_send_channel) {
            m_send_channel->send(
                Http2OutgoingFrame::segmented(std::move(header_bytes), std::string(header_block), waiter));
        } else {
            m_send_queue->push_back(
                Http2OutgoingFrame::segmented(std::move(header_bytes), std::string(header_block), waiter));
        }
    }

    void sendEncodedHeadersInternal(std::string&& header_block,
                                    bool end_stream,
                                    bool end_headers,
                                    const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) return;

        auto header_bytes = Http2FrameBuilder::headersHeaderBytes(m_stream_id,
                                                                  header_block.size(),
                                                                  end_stream,
                                                                  end_headers);

        onHeadersSent(end_stream);
        if (m_send_channel) {
            m_send_channel->send(
                Http2OutgoingFrame::segmented(std::move(header_bytes), std::move(header_block), waiter));
        } else {
            m_send_queue->push_back(
                Http2OutgoingFrame::segmented(std::move(header_bytes), std::move(header_block), waiter));
        }
    }

    void sendEncodedHeadersInternal(std::shared_ptr<const std::string> header_block,
                                    bool end_stream,
                                    bool end_headers,
                                    const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) return;

        auto payload = header_block ? std::move(header_block) : emptySharedPayload();
        auto header_bytes = Http2FrameBuilder::headersHeaderBytes(m_stream_id,
                                                                  payload->size(),
                                                                  end_stream,
                                                                  end_headers);

        onHeadersSent(end_stream);
        if (m_send_channel) {
            m_send_channel->send(
                Http2OutgoingFrame::segmentedShared(std::move(header_bytes), std::move(payload), waiter));
        } else {
            m_send_queue->push_back(
                Http2OutgoingFrame::segmentedShared(std::move(header_bytes), std::move(payload), waiter));
        }
    }

    void sendDataInternal(const std::string& data,
                          bool end_stream,
                          const Http2OutgoingFrame::WaiterPtr& waiter) {
        queueDataForSend(std::make_shared<const std::string>(data), end_stream, waiter);
    }

    void sendDataInternal(std::string&& data,
                          bool end_stream,
                          const Http2OutgoingFrame::WaiterPtr& waiter) {
        queueDataForSend(
            std::make_shared<const std::string>(std::move(data)), end_stream, waiter);
    }

    void sendDataInternal(std::shared_ptr<const std::string> data,
                          bool end_stream,
                          const Http2OutgoingFrame::WaiterPtr& waiter) {
        queueDataForSend(std::move(data), end_stream, waiter);
    }

    void sendRstStreamInternal(Http2ErrorCode error,
                               const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) return;

        auto bytes = Http2FrameBuilder::rstStreamBytes(m_stream_id, error);

        onRstStreamSent();
        if (m_send_channel) {
            m_send_channel->send(Http2OutgoingFrame{std::move(bytes), waiter});
        } else {
            m_send_queue->push_back(Http2OutgoingFrame{std::move(bytes), waiter});
        }
    }

    static std::shared_ptr<const std::string> emptySharedPayload() {
        static const auto payload = std::make_shared<const std::string>();
        return payload;
    }

    static std::vector<Http2OutgoingFrame>& outgoingScratch(size_t min_capacity) {
        static thread_local std::vector<Http2OutgoingFrame> scratch;
        scratch.clear();
        if (scratch.capacity() < min_capacity) {
            scratch.reserve(min_capacity);
        }
        return scratch;
    }

    void sendHeadersAndDataInternal(const std::vector<Http2HeaderField>& headers,
                                    std::string data,
                                    bool end_headers,
                                    const Http2OutgoingFrame::WaiterPtr& waiter) {
        if ((!m_send_queue && !m_send_channel) || !m_encoder) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        auto header_block = m_encoder->encode(headers);
        sendEncodedHeadersAndDataInternal(std::move(header_block), std::move(data), end_headers, waiter);
    }

    void sendHeadersAndDataChunksInternal(const std::vector<Http2HeaderField>& headers,
                                          std::vector<std::string>&& chunks,
                                          bool end_headers,
                                          const Http2OutgoingFrame::WaiterPtr& waiter) {
        if ((!m_send_queue && !m_send_channel) || !m_encoder) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        auto header_block = m_encoder->encode(headers);
        sendEncodedHeadersAndDataChunksInternal(
            std::move(header_block), std::move(chunks), end_headers, waiter);
    }

    void sendEncodedHeadersAndDataInternal(std::string&& header_block,
                                           std::string&& data,
                                           bool end_headers,
                                           const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        const bool header_end_stream = data.empty();
        auto header_bytes = Http2FrameBuilder::headersHeaderBytes(
            m_stream_id, header_block.size(), header_end_stream, end_headers);
        onHeadersSent(header_end_stream);

        const bool can_send_data =
            !data.empty() && m_send_window >= static_cast<int32_t>(data.size());

        if (m_send_channel) {
            if (!can_send_data) {
                m_send_channel->send(
                    Http2OutgoingFrame::segmented(
                        std::move(header_bytes), std::move(header_block), waiter));
                return;
            }

            auto& outgoing = outgoingScratch(2);
            outgoing.push_back(
                Http2OutgoingFrame::segmented(std::move(header_bytes), std::move(header_block)));

            auto data_header = Http2FrameBuilder::dataHeaderBytes(m_stream_id, data.size(), true);
            m_send_window -= static_cast<int32_t>(data.size());
            onDataSent(true);
            outgoing.push_back(
                Http2OutgoingFrame::segmented(std::move(data_header), std::move(data)));
            if (waiter) {
                outgoing.back().waiter = waiter;
            }
            m_send_channel->sendBatch(std::move(outgoing));
            return;
        }

        m_send_queue->push_back(
            Http2OutgoingFrame::segmented(std::move(header_bytes), std::move(header_block)));

        if (can_send_data) {
            auto data_header = Http2FrameBuilder::dataHeaderBytes(m_stream_id, data.size(), true);
            m_send_window -= static_cast<int32_t>(data.size());
            onDataSent(true);
            m_send_queue->push_back(
                Http2OutgoingFrame::segmented(std::move(data_header), std::move(data)));
        }

        if (waiter) {
            m_send_queue->back().waiter = waiter;
        }
    }

    void sendEncodedHeadersAndDataInternal(std::shared_ptr<const std::string> header_block,
                                           std::string&& data,
                                           bool end_headers,
                                           const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        auto payload = header_block ? std::move(header_block) : emptySharedPayload();
        const bool header_end_stream = data.empty();
        auto header_bytes = Http2FrameBuilder::headersHeaderBytes(
            m_stream_id, payload->size(), header_end_stream, end_headers);
        onHeadersSent(header_end_stream);

        const bool can_send_data =
            !data.empty() && m_send_window >= static_cast<int32_t>(data.size());

        if (m_send_channel) {
            if (!can_send_data) {
                m_send_channel->send(
                    Http2OutgoingFrame::segmentedShared(
                        std::move(header_bytes), std::move(payload), waiter));
                return;
            }

            auto& outgoing = outgoingScratch(2);
            outgoing.push_back(
                Http2OutgoingFrame::segmentedShared(std::move(header_bytes), std::move(payload)));

            auto data_header = Http2FrameBuilder::dataHeaderBytes(m_stream_id, data.size(), true);
            m_send_window -= static_cast<int32_t>(data.size());
            onDataSent(true);
            outgoing.push_back(
                Http2OutgoingFrame::segmented(std::move(data_header), std::move(data)));
            if (waiter) {
                outgoing.back().waiter = waiter;
            }
            m_send_channel->sendBatch(std::move(outgoing));
            return;
        }

        m_send_queue->push_back(
            Http2OutgoingFrame::segmentedShared(std::move(header_bytes), std::move(payload)));

        if (can_send_data) {
            auto data_header = Http2FrameBuilder::dataHeaderBytes(m_stream_id, data.size(), true);
            m_send_window -= static_cast<int32_t>(data.size());
            onDataSent(true);
            m_send_queue->push_back(
                Http2OutgoingFrame::segmented(std::move(data_header), std::move(data)));
        }

        if (waiter) {
            m_send_queue->back().waiter = waiter;
        }
    }

    void sendEncodedHeadersAndDataChunksInternal(std::string&& header_block,
                                                 std::vector<std::string>&& chunks,
                                                 bool end_headers,
                                                 const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        if (chunks.size() == 1) {
            sendEncodedHeadersAndDataInternal(
                std::move(header_block), std::move(chunks.front()), end_headers, waiter);
            return;
        }

        const bool header_end_stream = chunks.empty();
        auto header_bytes = Http2FrameBuilder::headersHeaderBytes(
            m_stream_id, header_block.size(), header_end_stream, end_headers);
        onHeadersSent(header_end_stream);

        if (m_send_channel) {
            auto& outgoing = outgoingScratch(1 + chunks.size());
            outgoing.push_back(
                Http2OutgoingFrame::segmented(std::move(header_bytes), std::move(header_block)));

            if (!chunks.empty()) {
                for (size_t i = 0; i < chunks.size(); ++i) {
                    auto& chunk = chunks[i];
                    if (m_send_window < static_cast<int32_t>(chunk.size())) {
                        continue;
                    }

                    const bool last = (i + 1 == chunks.size());
                    auto data_header = Http2FrameBuilder::dataHeaderBytes(m_stream_id, chunk.size(), last);
                    m_send_window -= static_cast<int32_t>(chunk.size());
                    if (last) {
                        onDataSent(true);
                    }
                    outgoing.push_back(
                        Http2OutgoingFrame::segmented(std::move(data_header), std::move(chunk)));
                }
            }

            if (waiter) {
                outgoing.back().waiter = waiter;
            }
            m_send_channel->sendBatch(std::move(outgoing));
            return;
        }

        m_send_queue->push_back(
            Http2OutgoingFrame::segmented(std::move(header_bytes), std::move(header_block)));

        if (!chunks.empty()) {
            for (size_t i = 0; i < chunks.size(); ++i) {
                auto& chunk = chunks[i];
                if (m_send_window < static_cast<int32_t>(chunk.size())) {
                    continue;
                }

                const bool last = (i + 1 == chunks.size());
                auto data_header = Http2FrameBuilder::dataHeaderBytes(m_stream_id, chunk.size(), last);
                m_send_window -= static_cast<int32_t>(chunk.size());
                if (last) {
                    onDataSent(true);
                }
                m_send_queue->push_back(
                    Http2OutgoingFrame::segmented(std::move(data_header), std::move(chunk)));
            }
        }

        if (waiter) {
            m_send_queue->back().waiter = waiter;
        }
    }

    void sendDataBatchInternal(const std::vector<std::string>& chunks,
                               bool end_stream,
                               const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        std::vector<Http2OutgoingFrame> outgoing;
        outgoing.reserve(chunks.size() + (chunks.empty() && end_stream ? 1 : 0));

        if (chunks.empty()) {
            if (end_stream) {
                onDataSent(true);
                outgoing.emplace_back(Http2FrameBuilder::dataBytes(m_stream_id, std::string_view{}, true));
            }
        } else {
            for (size_t i = 0; i < chunks.size(); ++i) {
                const auto& chunk = chunks[i];
                if (m_send_window < static_cast<int32_t>(chunk.size())) {
                    continue;
                }
                const bool last = end_stream && (i + 1 == chunks.size());
                m_send_window -= static_cast<int32_t>(chunk.size());
                if (last) {
                    onDataSent(true);
                }
                outgoing.emplace_back(Http2FrameBuilder::dataBytes(m_stream_id, chunk, last));
            }
        }

        if (outgoing.empty()) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        if (waiter) {
            outgoing.back().waiter = waiter;
        }

        if (m_send_channel) {
            m_send_channel->sendBatch(std::move(outgoing));
        } else {
            m_send_queue->insert(m_send_queue->end(),
                                 std::make_move_iterator(outgoing.begin()),
                                 std::make_move_iterator(outgoing.end()));
        }
    }

    void sendDataChunksInternal(std::vector<std::string>&& chunks,
                                bool end_stream,
                                const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        if (chunks.size() == 1) {
            sendDataInternal(std::move(chunks.front()), end_stream, waiter);
            return;
        }

        std::vector<Http2OutgoingFrame> outgoing;
        outgoing.reserve(chunks.size() + (chunks.empty() && end_stream ? 1 : 0));

        if (chunks.empty()) {
            if (end_stream) {
                onDataSent(true);
                outgoing.emplace_back(Http2FrameBuilder::dataBytes(m_stream_id, std::string_view{}, true));
            }
        } else {
            for (size_t i = 0; i < chunks.size(); ++i) {
                auto& chunk = chunks[i];
                if (m_send_window < static_cast<int32_t>(chunk.size())) {
                    continue;
                }

                const bool last = end_stream && (i + 1 == chunks.size());
                auto header_bytes = Http2FrameBuilder::dataHeaderBytes(m_stream_id, chunk.size(), last);
                m_send_window -= static_cast<int32_t>(chunk.size());
                if (last) {
                    onDataSent(true);
                }
                outgoing.push_back(
                    Http2OutgoingFrame::segmented(std::move(header_bytes), std::move(chunk)));
            }
        }

        if (outgoing.empty()) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        if (waiter) {
            outgoing.back().waiter = waiter;
        }

        if (m_send_channel) {
            m_send_channel->sendBatch(std::move(outgoing));
        } else {
            m_send_queue->insert(m_send_queue->end(),
                                 std::make_move_iterator(outgoing.begin()),
                                 std::make_move_iterator(outgoing.end()));
        }
    }

    void sendFrameBatchInternal(std::vector<Http2Frame::uptr> frames,
                                const Http2OutgoingFrame::WaiterPtr& waiter) {
        if (!m_send_queue && !m_send_channel) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        std::vector<Http2OutgoingFrame> outgoing;
        outgoing.reserve(frames.size());

        for (auto& frame : frames) {
            if (!frame) {
                continue;
            }

            frame->header().stream_id = m_stream_id;

            if (frame->isHeaders()) {
                onHeadersSent(frame->asHeaders()->isEndStream());
            } else if (frame->isData()) {
                auto* data = frame->asData();
                if (m_send_window < static_cast<int32_t>(data->data().size())) {
                    continue;
                }
                m_send_window -= static_cast<int32_t>(data->data().size());
                if (data->isEndStream()) {
                    onDataSent(true);
                }
            } else if (frame->isRstStream()) {
                onRstStreamSent();
            }

            outgoing.push_back(Http2OutgoingFrame{std::move(frame)});
        }

        if (outgoing.empty()) {
            if (waiter) {
                waiter->notify();
            }
            return;
        }

        if (waiter) {
            outgoing.back().waiter = waiter;
        }

        if (m_send_channel) {
            m_send_channel->sendBatch(std::move(outgoing));
            return;
        }

        m_send_queue->insert(
            m_send_queue->end(),
            std::make_move_iterator(outgoing.begin()),
            std::make_move_iterator(outgoing.end()));
    }
};

class Http2StreamPool
{
public:
    Http2StreamPool()
        : m_state(std::make_shared<State>())
    {
    }

    Http2Stream::ptr acquire(uint32_t stream_id) {
        Http2Stream* stream = nullptr;
        if (!m_state->free_list.empty()) {
            stream = m_state->free_list.back();
            m_state->free_list.pop_back();
            stream->resetForReuse(stream_id);
        } else {
            stream = new Http2Stream(stream_id);
        }

        auto state = m_state;
        return Http2Stream::ptr(stream, [state = std::move(state)](Http2Stream* ptr) mutable {
            if (!ptr) {
                return;
            }
            state->free_list.push_back(ptr);
        });
    }

    size_t available() const {
        return m_state->free_list.size();
    }

private:
    struct State {
        ~State() {
            for (auto* stream : free_list) {
                delete stream;
            }
        }

        std::vector<Http2Stream*> free_list;
    };

    std::shared_ptr<State> m_state;
};

} // namespace galay::http2

#endif // GALAY_HTTP2_STREAM_H
