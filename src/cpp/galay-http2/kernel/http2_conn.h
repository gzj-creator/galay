/**
 * @file http2_conn.h
 * @brief HTTP/2 连接实现，管理帧收发、流控制和 HPACK 编解码
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 Http2ConnImpl 模板类，封装 HTTP/2 连接的完整生命周期，
 *          包括 SETTINGS 握手、流管理、流量控制和帧读写。
 *          支持 AsyncTcpSocket 和 SslSocket 两种底层传输。
 */

#ifndef GALAY_HTTP2_CONN_H
#define GALAY_HTTP2_CONN_H

#include "http2_stream.h"
#include "h2_core.h"
#include "../server/h2_static_file.h"
#include "../protoc/http2_base.h"
#include "../protoc/http2_frame.h"
#include "../protoc/http2_hpack.h"
#include "../protoc/http2_error.h"
#include "../../galay-http/common/iovec_utils.h"
#include "../../galay-http/kernel/http_conn.h"
#include "../../galay-utils/buffer/bytes.hpp"
#include "../../galay-utils/buffer/ring_buffer.hpp"
#include "../../galay-utils/encoding/base64.hpp"
#include "../../galay-kernel/common/error.h"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/timeout.hpp"
#include "../../galay-kernel/async/async_tcp.h"
#include <unordered_map>
#include <memory>
#include <expected>
#include <array>
#include <functional>
#include <cstring>
#include <chrono>
#include <string_view>
#include <type_traits>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-ssl/async/ssl_await.h"
#include "../../galay-ssl/async/ssl_socket.h"
#endif

namespace galay::http2
{

using namespace galay::kernel;
using ::galay::utils::Bytes;
using ::galay::utils::RingBuffer;
using ::galay::utils::RingBufferBackendStrategy;

// 前向声明 StreamManager
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class Http2StreamManagerImpl;

// 类型特征：检测是否是 SslSocket
template<typename T>
struct is_ssl_socket : std::false_type {};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<>
struct is_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

template<typename T>
inline constexpr bool is_ssl_socket_v = is_ssl_socket<T>::value;

struct Http2RawFrameView
{
    Http2FrameHeader header{};
    std::string owned_bytes;
    const char* borrowed_bytes = nullptr;
    size_t frame_size = 0;
    size_t payload_offset = kHttp2FrameHeaderLength;
    size_t payload_size = 0;

    Http2RawFrameView() = default;

    Http2RawFrameView(Http2FrameHeader frame_header,
                      const char* frame_bytes,
                      size_t total_frame_size,
                      size_t payload_begin,
                      size_t payload_length)
        : header(frame_header)
        , borrowed_bytes(frame_bytes)
        , frame_size(total_frame_size)
        , payload_offset(payload_begin)
        , payload_size(payload_length)
    {
    }

    Http2RawFrameView(Http2FrameHeader frame_header,
                      std::string frame_bytes,
                      size_t payload_begin,
                      size_t payload_length)
        : header(frame_header)
        , owned_bytes(std::move(frame_bytes))
        , frame_size(owned_bytes.size())
        , payload_offset(payload_begin)
        , payload_size(payload_length)
    {
    }

    std::string_view bytes() const {
        if (!owned_bytes.empty()) {
            return std::string_view(owned_bytes.data(), owned_bytes.size());
        }
        if (borrowed_bytes == nullptr || frame_size == 0) {
            return {};
        }
        return std::string_view(borrowed_bytes, frame_size);
    }

    std::string_view payload() const {
        auto view = bytes();
        if (payload_offset > view.size()) {
            return {};
        }
        return view.substr(payload_offset, payload_size);
    }

    uint32_t stream_id() const { return header.stream_id; }
    bool is_headers() const { return header.type == Http2FrameType::Headers; }
    bool is_data() const { return header.type == Http2FrameType::Data; }
    bool is_continuation() const { return header.type == Http2FrameType::Continuation; }
    bool is_priority() const { return header.type == Http2FrameType::Priority; }
    bool is_window_update() const { return header.type == Http2FrameType::WindowUpdate; }
    bool is_rst_stream() const { return header.type == Http2FrameType::RstStream; }
    bool is_connection_frame() const {
        return header.stream_id == 0 &&
               (header.type == Http2FrameType::Settings ||
                header.type == Http2FrameType::Ping ||
                header.type == Http2FrameType::GoAway ||
                header.type == Http2FrameType::WindowUpdate);
    }
    bool end_stream() const { return (header.flags & Http2FrameFlags::kEndStream) != 0; }
    bool end_headers() const { return (header.flags & Http2FrameFlags::kEndHeaders) != 0; }
};

/**
 * @brief HTTP/2 连接设置
 */
struct Http2Settings
{
    uint32_t header_table_size = kDefaultHeaderTableSize;
    uint32_t enable_push = kDefaultEnablePush;
    uint32_t max_concurrent_streams = kDefaultMaxConcurrentStreams;
    uint32_t initial_window_size = kDefaultInitialWindowSize;
    uint32_t max_frame_size = kDefaultMaxFrameSize;
    uint32_t max_header_list_size = kDefaultMaxHeaderListSize;
    
    Http2ErrorCode apply_settings(const Http2SettingsFrame& frame) {
        for (const auto& setting : frame.settings()) {
            switch (setting.id) {
                case Http2SettingsId::HeaderTableSize:
                    header_table_size = setting.value;
                    break;
                case Http2SettingsId::EnablePush:
                    if (setting.value > 1) return Http2ErrorCode::ProtocolError;
                    enable_push = setting.value;
                    break;
                case Http2SettingsId::MaxConcurrentStreams:
                    max_concurrent_streams = setting.value;
                    break;
                case Http2SettingsId::InitialWindowSize:
                    if (setting.value > 2147483647u) return Http2ErrorCode::FlowControlError;
                    initial_window_size = setting.value;
                    break;
                case Http2SettingsId::MaxFrameSize:
                    if (setting.value < 16384 || setting.value > 16777215) return Http2ErrorCode::ProtocolError;
                    max_frame_size = setting.value;
                    break;
                case Http2SettingsId::MaxHeaderListSize:
                    max_header_list_size = setting.value;
                    break;
            }
        }
        return Http2ErrorCode::NoError;
    }
    
    template<typename Config>
    void from(const Config& config) {
        if constexpr (requires { config.header_table_size; })
            header_table_size = config.header_table_size;
        if constexpr (requires { config.enable_push; }) {
            if constexpr (std::is_same_v<decltype(config.enable_push), const bool>)
                enable_push = config.enable_push ? 1 : 0;
            else
                enable_push = config.enable_push;
        }
        if constexpr (requires { config.max_concurrent_streams; })
            max_concurrent_streams = config.max_concurrent_streams;
        if constexpr (requires { config.initial_window_size; })
            initial_window_size = config.initial_window_size;
        if constexpr (requires { config.max_frame_size; })
            max_frame_size = config.max_frame_size;
        if constexpr (requires { config.max_header_list_size; })
            max_header_list_size = config.max_header_list_size;
    }

    Http2SettingsFrame to_frame() const {
        Http2SettingsFrame frame;
        frame.add_setting(Http2SettingsId::HeaderTableSize, header_table_size);
        frame.add_setting(Http2SettingsId::EnablePush, enable_push);
        frame.add_setting(Http2SettingsId::MaxConcurrentStreams, max_concurrent_streams);
        frame.add_setting(Http2SettingsId::InitialWindowSize, initial_window_size);
        frame.add_setting(Http2SettingsId::MaxFrameSize, max_frame_size);
        frame.add_setting(Http2SettingsId::MaxHeaderListSize, max_header_list_size);
        return frame;
    }
};

struct Http2FlowControlUpdate
{
    uint32_t conn_increment = 0;
    uint32_t stream_increment = 0;
};

using Http2FlowControlStrategy = std::function<Http2FlowControlUpdate(
    int32_t conn_recv_window,
    int32_t stream_recv_window,
    uint32_t target_window,
    size_t data_size)>;

struct Http2RuntimeConfig
{
    bool ping_enabled = true;
    std::chrono::milliseconds ping_interval{30000};
    std::chrono::milliseconds ping_timeout{10000};
    std::chrono::milliseconds settings_ack_timeout{10000};
    std::chrono::milliseconds graceful_shutdown_rtt{100};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    uint32_t flow_control_target_window = kDefaultInitialWindowSize;
    Http2FlowControlStrategy flow_control_strategy;
    std::vector<H2StaticRoute> static_routes;
    std::vector<H2StaticFileMount> static_file_mounts;

    template<typename Config>
    void from(const Config& config) {
        if constexpr (requires { config.ping_enabled; }) {
            ping_enabled = config.ping_enabled;
        }
        if constexpr (requires { config.ping_interval; }) {
            ping_interval = config.ping_interval;
        }
        if constexpr (requires { config.ping_timeout; }) {
            ping_timeout = config.ping_timeout;
        }
        if constexpr (requires { config.settings_ack_timeout; }) {
            settings_ack_timeout = config.settings_ack_timeout;
        }
        if constexpr (requires { config.graceful_shutdown_rtt; }) {
            graceful_shutdown_rtt = config.graceful_shutdown_rtt;
        }
        if constexpr (requires { config.graceful_shutdown_timeout; }) {
            graceful_shutdown_timeout = config.graceful_shutdown_timeout;
        }
        if constexpr (requires { config.flow_control_target_window; }) {
            flow_control_target_window = config.flow_control_target_window;
        }
        if constexpr (requires { config.flow_control_strategy; }) {
            flow_control_strategy = config.flow_control_strategy;
        }
        if constexpr (requires { config.static_routes; }) {
            static_routes = config.static_routes;
            for (auto& route : static_routes) {
                prepare_h2_static_route(route);
            }
        }
        if constexpr (requires { config.static_file_mounts; }) {
            static_file_mounts = config.static_file_mounts;
            for (auto& mount : static_file_mounts) {
                if (!mount.cache) {
                    mount.cache = std::make_shared<H2StaticFileCache>(mount.config);
                }
            }
        }
    }
};

// 前向声明
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class Http2ConnImpl;

namespace detail {

template<typename ResultT>
struct ExpectedTraits;

template<typename T, typename E>
struct ExpectedTraits<std::expected<T, E>> {
    using value_type = T;
    using error_type = E;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<typename ResultT>
struct Http2SslResult;

template<typename T, typename E>
struct Http2SslResult<std::expected<T, E>> {
    using type = std::expected<T, Http2Error>;
};

template<typename ResultT>
using Http2SslResultT = typename Http2SslResult<ResultT>::type;

template<typename ResultT>
Http2SslResultT<ResultT> to_ssl_http2_result(ResultT result) {
    using ValueT = typename ExpectedTraits<ResultT>::value_type;

    if (!result) {
        return std::unexpected(Http2Error(result.error()));
    }
    if constexpr (std::is_void_v<ValueT>) {
        return {};
    } else {
        return Http2SslResultT<ResultT>(std::move(result.value()));
    }
}
#endif

template<typename ResultT, typename InnerResultT>
ResultT to_outer_http2_result(InnerResultT result) {
    using ValueT = typename ExpectedTraits<ResultT>::value_type;
    using OuterErrorT = typename ExpectedTraits<ResultT>::error_type;
    using InnerErrorT = typename ExpectedTraits<InnerResultT>::error_type;

    if (!result) {
        if constexpr (std::is_same_v<OuterErrorT, Http2ErrorCode> &&
                      std::is_same_v<InnerErrorT, Http2Error>) {
            return std::unexpected(result.error().code());
        } else {
            return std::unexpected(OuterErrorT(result.error()));
        }
    }
    if constexpr (std::is_void_v<ValueT>) {
        return {};
    } else {
        return ResultT(std::move(result.value()));
    }
}

struct Http2BufferedFrameStatus {
    Http2FrameHeader header{};
    size_t total_frame_size = 0;
    bool complete = false;
    std::optional<Http2ErrorCode> error;
};

inline bool decode_frame_header(const struct iovec* iovecs,
                              size_t iov_count,
                              Http2FrameHeader& header) {
    if (iovecs == nullptr || iov_count == 0) {
        return false;
    }

    const auto* first_segment = IoVecWindow::first_non_empty(iovecs, iov_count);
    if (first_segment == nullptr) {
        return false;
    }

    if (first_segment->iov_len >= kHttp2FrameHeaderLength) {
        header = Http2FrameHeader::deserialize(
            static_cast<const uint8_t*>(first_segment->iov_base));
        return true;
    }

    uint8_t header_buf[kHttp2FrameHeaderLength];
    if (IoVecBytes::copy_prefix(iovecs, iov_count, header_buf, kHttp2FrameHeaderLength)
        < kHttp2FrameHeaderLength) {
        return false;
    }

    header = Http2FrameHeader::deserialize(header_buf);
    return true;
}

inline bool is_known_http2_frame_type(Http2FrameType type) {
    switch (type) {
        case Http2FrameType::Data:
        case Http2FrameType::Headers:
        case Http2FrameType::Priority:
        case Http2FrameType::RstStream:
        case Http2FrameType::Settings:
        case Http2FrameType::PushPromise:
        case Http2FrameType::Ping:
        case Http2FrameType::GoAway:
        case Http2FrameType::WindowUpdate:
        case Http2FrameType::Continuation:
            return true;
        default:
            return false;
    }
}

template<RingBufferBackendStrategy Strategy>
inline Http2BufferedFrameStatus inspect_buffered_frame(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                                                     uint32_t max_frame_size) {
    Http2BufferedFrameStatus status;
    if (ring_buffer.readable() < kHttp2FrameHeaderLength) {
        return status;
    }

    const auto read_iovecs = borrow_read_iovecs(ring_buffer);
    if (read_iovecs.empty()) {
        return status;
    }

    if (!decode_frame_header(read_iovecs.data(), read_iovecs.size(), status.header)) {
        return status;
    }

    if (status.header.length > max_frame_size) {
        status.error = Http2ErrorCode::FrameSizeError;
        return status;
    }

    status.total_frame_size = kHttp2FrameHeaderLength + static_cast<size_t>(status.header.length);
    status.complete = ring_buffer.readable() >= status.total_frame_size;
    return status;
}

template<RingBufferBackendStrategy Strategy>
inline std::expected<Http2Frame::uptr, Http2ErrorCode>
parse_single_buffered_frame(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                         uint32_t max_frame_size,
                         std::vector<uint8_t>& scratch) {
    while (true) {
        const auto status = inspect_buffered_frame(ring_buffer, max_frame_size);
        if (status.error.has_value()) {
            return std::unexpected(*status.error);
        }
        if (!status.complete) {
            return std::unexpected(Http2ErrorCode::NoError);
        }
        if (!is_known_http2_frame_type(status.header.type)) {
            ring_buffer.consume(status.total_frame_size);
            continue;
        }

        const auto read_iovecs = borrow_read_iovecs(ring_buffer);
        const auto* first_segment = IoVecWindow::first_non_empty(read_iovecs);

        std::expected<Http2Frame::uptr, Http2ErrorCode> frame_result;
        if (first_segment != nullptr && first_segment->iov_len >= status.total_frame_size) {
            frame_result = Http2FrameParser::parse_frame(
                static_cast<const uint8_t*>(first_segment->iov_base),
                status.total_frame_size);
        } else {
            if (scratch.size() < status.total_frame_size) {
                scratch.resize(status.total_frame_size);
            }
            if (IoVecBytes::copy_prefix(read_iovecs.data(),
                                       read_iovecs.size(),
                                       scratch.data(),
                                       status.total_frame_size) < status.total_frame_size) {
                return std::unexpected(Http2ErrorCode::ProtocolError);
            }
            frame_result = Http2FrameParser::parse_frame(scratch.data(), status.total_frame_size);
        }

        if (!frame_result.has_value()) {
            return std::unexpected(frame_result.error());
        }

        ring_buffer.consume(status.total_frame_size);
        return frame_result;
    }
}

template<RingBufferBackendStrategy Strategy>
inline std::expected<std::vector<Http2Frame::uptr>, Http2ErrorCode>
parse_buffered_frame_batch(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                        uint32_t max_frame_size,
                        size_t max_frames,
                        std::vector<uint8_t>& scratch) {
    std::vector<Http2Frame::uptr> frames;
    const size_t reserve_hint =
        (max_frames == std::numeric_limits<size_t>::max())
            ? 16
            : std::min<size_t>(max_frames, 256);
    frames.reserve(reserve_hint);

    while (frames.size() < max_frames) {
        const auto status = inspect_buffered_frame(ring_buffer, max_frame_size);
        if (status.error.has_value()) {
            return std::unexpected(*status.error);
        }
        if (!status.complete) {
            break;
        }
        if (!is_known_http2_frame_type(status.header.type)) {
            ring_buffer.consume(status.total_frame_size);
            continue;
        }

        const auto read_iovecs = borrow_read_iovecs(ring_buffer);
        const auto* first_segment = IoVecWindow::first_non_empty(read_iovecs);

        std::expected<Http2Frame::uptr, Http2ErrorCode> frame_result;
        if (first_segment != nullptr && first_segment->iov_len >= status.total_frame_size) {
            frame_result = Http2FrameParser::parse_frame(
                static_cast<const uint8_t*>(first_segment->iov_base),
                status.total_frame_size);
        } else {
            if (scratch.size() < status.total_frame_size) {
                scratch.resize(status.total_frame_size);
            }
            if (IoVecBytes::copy_prefix(read_iovecs.data(),
                                       read_iovecs.size(),
                                       scratch.data(),
                                       status.total_frame_size) < status.total_frame_size) {
                return std::unexpected(Http2ErrorCode::ProtocolError);
            }
            frame_result = Http2FrameParser::parse_frame(scratch.data(), status.total_frame_size);
        }

        if (!frame_result.has_value()) {
            return std::unexpected(frame_result.error());
        }

        ring_buffer.consume(status.total_frame_size);
        frames.push_back(std::move(*frame_result));
    }

    return frames;
}

template<RingBufferBackendStrategy Strategy>
inline std::expected<std::vector<Http2RawFrameView>, Http2ErrorCode>
parse_buffered_frame_view_batch(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                            uint32_t max_frame_size,
                            size_t max_frames) {
    std::vector<Http2RawFrameView> frames;
    const size_t reserve_hint =
        (max_frames == std::numeric_limits<size_t>::max())
            ? 16
            : std::min<size_t>(max_frames, 256);
    frames.reserve(reserve_hint);

    while (frames.size() < max_frames) {
        const auto status = inspect_buffered_frame(ring_buffer, max_frame_size);
        if (status.error.has_value()) {
            return std::unexpected(*status.error);
        }
        if (!status.complete) {
            break;
        }
        if (!is_known_http2_frame_type(status.header.type)) {
            ring_buffer.consume(status.total_frame_size);
            continue;
        }

        const auto read_iovecs = borrow_read_iovecs(ring_buffer);
        const auto* first_segment = IoVecWindow::first_non_empty(read_iovecs);
        if (first_segment != nullptr && first_segment->iov_len >= status.total_frame_size) {
            frames.emplace_back(status.header,
                                static_cast<const char*>(first_segment->iov_base),
                                status.total_frame_size,
                                kHttp2FrameHeaderLength,
                                status.header.length);
        } else {
            std::string frame_bytes;
            frame_bytes.resize(status.total_frame_size);
            if (IoVecBytes::copy_prefix(read_iovecs.data(),
                                       read_iovecs.size(),
                                       reinterpret_cast<uint8_t*>(frame_bytes.data()),
                                       status.total_frame_size) < status.total_frame_size) {
                return std::unexpected(Http2ErrorCode::ProtocolError);
            }
            frames.emplace_back(status.header,
                                std::move(frame_bytes),
                                kHttp2FrameHeaderLength,
                                status.header.length);
        }

        ring_buffer.consume(status.total_frame_size);
    }

    return frames;
}

template<typename ValueT, RingBufferBackendStrategy Strategy>
struct Http2ReadStateBase {
    using ResultType = std::expected<ValueT, Http2ErrorCode>;

    Http2ReadStateBase(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                       Http2Settings& peer_settings,
                       bool* peer_closed = nullptr,
                       std::string* last_error_msg = nullptr,
                       const bool* closing = nullptr)
        : m_ring_buffer(&ring_buffer)
        , m_peer_settings(&peer_settings)
        , m_peer_closed(peer_closed)
        , m_last_error_msg(last_error_msg)
        , m_closing(closing) {}

    bool has_result() const { return m_result.has_value(); }

    ResultType take_result() { return std::move(*m_result); }

    bool complete_if_closing() {
        if (!(m_closing != nullptr && *m_closing)) {
            return false;
        }

        const auto status = inspect_buffered_frame(*m_ring_buffer, m_peer_settings->max_frame_size);
        if (status.error.has_value()) {
            set_protocol_error(*status.error, "frame too large");
            return true;
        }
        if (status.complete) {
            return false;
        }

        set_protocol_error(Http2ErrorCode::ProtocolError, "Connection closing");
        return true;
    }

    bool prepare_recv_window() {
        m_write_iovecs.capture_write(*m_ring_buffer);
        const size_t compact_count =
            compact_iovecs(m_write_iovecs.storage(), m_write_iovecs.size());
        m_write_iovecs.set_count(compact_count);
        if (m_write_iovecs.empty()) {
            set_protocol_error(Http2ErrorCode::ProtocolError, "RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> is full");
            return false;
        }
        return true;
    }

    bool prepare_recv_window(char*& buffer, size_t& length) {
        if (!prepare_recv_window()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bind_first_non_empty(m_write_iovecs, buffer, length)) {
            set_protocol_error(Http2ErrorCode::ProtocolError, "RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> is full");
            return false;
        }
        return true;
    }

    const struct iovec* recv_iovecs_data() const { return m_write_iovecs.data(); }
    size_t recv_iovecs_count() const { return m_write_iovecs.size(); }

    void on_bytes_received(size_t recv_bytes) {
        m_ring_buffer->produce(recv_bytes);
        clear_last_read_error();
    }

    void set_recv_error(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError) && m_peer_closed) {
            *m_peer_closed = true;
        }
        assign_last_read_error(io_error.message());
        m_result.emplace(std::unexpected(Http2ErrorCode::ProtocolError));
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    void set_ssl_recv_error(const galay::ssl::SslError& error) {
        const Http2Error http2_error(error);
        set_protocol_error(http2_error.code(), http2_error.message());
    }
#endif

    void set_protocol_error(Http2ErrorCode code, std::string_view msg) {
        if (code == Http2ErrorCode::ProtocolError && msg == "peer closed" && m_peer_closed) {
            *m_peer_closed = true;
        }
        assign_last_read_error(msg);
        m_result.emplace(std::unexpected(code));
    }

protected:
    void complete(ResultType result) {
        m_result.emplace(std::move(result));
    }

    void assign_last_read_error(std::string_view msg) {
        if (m_last_error_msg) {
            m_last_error_msg->assign(msg.data(), msg.size());
        }
    }

    void clear_last_read_error() {
        if (m_last_error_msg) {
            m_last_error_msg->clear();
        }
    }

    RingBuffer<Strategy, std::dynamic_extent>* m_ring_buffer = nullptr;
    Http2Settings* m_peer_settings = nullptr;
    bool* m_peer_closed = nullptr;
    std::string* m_last_error_msg = nullptr;
    const bool* m_closing = nullptr;
    BorrowedIovecs<2> m_write_iovecs;
    std::vector<uint8_t> m_scratch;
    std::optional<ResultType> m_result;
};

template<RingBufferBackendStrategy Strategy>
struct Http2SingleFrameReadState : Http2ReadStateBase<Http2Frame::uptr, Strategy> {
    using Base = Http2ReadStateBase<Http2Frame::uptr, Strategy>;
    using Base::Base;

    bool parse_from_ring_buffer() {
        auto frame_result = parse_single_buffered_frame(
            *this->m_ring_buffer,
            this->m_peer_settings->max_frame_size,
            this->m_scratch);
        if (!frame_result.has_value()) {
            if (frame_result.error() == Http2ErrorCode::NoError) {
                return false;
            }
            this->complete(std::unexpected(frame_result.error()));
            return true;
        }

        this->complete(std::move(frame_result));
        return true;
    }
};

template<RingBufferBackendStrategy Strategy>
struct Http2FrameBatchReadState : Http2ReadStateBase<std::vector<Http2Frame::uptr>, Strategy> {
    using Base = Http2ReadStateBase<std::vector<Http2Frame::uptr>, Strategy>;

    Http2FrameBatchReadState(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                             Http2Settings& peer_settings,
                             size_t max_frames,
                             bool* peer_closed = nullptr,
                             std::string* last_error_msg = nullptr,
                             const bool* closing = nullptr)
        : Base(ring_buffer, peer_settings, peer_closed, last_error_msg, closing)
        , m_max_frames(max_frames) {}

    bool parse_from_ring_buffer() {
        auto frames_result = parse_buffered_frame_batch(
            *this->m_ring_buffer,
            this->m_peer_settings->max_frame_size,
            m_max_frames,
            this->m_scratch);
        if (!frames_result.has_value()) {
            this->complete(std::unexpected(frames_result.error()));
            return true;
        }
        if (frames_result->empty()) {
            return false;
        }

        this->complete(std::move(frames_result));
        return true;
    }

    size_t m_max_frames;
};

template<RingBufferBackendStrategy Strategy>
struct Http2FrameViewBatchReadState : Http2ReadStateBase<std::vector<Http2RawFrameView>, Strategy> {
    using Base = Http2ReadStateBase<std::vector<Http2RawFrameView>, Strategy>;

    Http2FrameViewBatchReadState(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                                 Http2Settings& peer_settings,
                                 size_t max_frames,
                                 bool* peer_closed = nullptr,
                                 std::string* last_error_msg = nullptr,
                                 const bool* closing = nullptr)
        : Base(ring_buffer, peer_settings, peer_closed, last_error_msg, closing)
        , m_max_frames(max_frames) {}

    bool parse_from_ring_buffer() {
        auto frames_result = parse_buffered_frame_view_batch(
            *this->m_ring_buffer,
            this->m_peer_settings->max_frame_size,
            m_max_frames);
        if (!frames_result.has_value()) {
            this->complete(std::unexpected(frames_result.error()));
            return true;
        }
        if (frames_result->empty()) {
            return false;
        }

        this->complete(std::move(frames_result));
        return true;
    }

    size_t m_max_frames;
};

template<typename StateT>
struct Http2TcpReadMachine {
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit Http2TcpReadMachine(StateT state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_state.has_result()) {
            return MachineAction<result_type>::complete(m_state.take_result());
        }
        if (m_state.parse_from_ring_buffer()) {
            return MachineAction<result_type>::complete(m_state.take_result());
        }
        if (m_state.complete_if_closing()) {
            return MachineAction<result_type>::complete(m_state.take_result());
        }
        if (!m_state.prepare_recv_window()) {
            return MachineAction<result_type>::complete(m_state.take_result());
        }

        return MachineAction<result_type>::wait_readv(
            m_state.recv_iovecs_data(),
            m_state.recv_iovecs_count());
    }

    void on_read(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state.set_recv_error(result.error());
            return;
        }
        if (result.value() == 0) {
            m_state.set_protocol_error(Http2ErrorCode::ProtocolError, "peer closed");
            return;
        }

        m_state.on_bytes_received(result.value());
    }

    void on_write(std::expected<size_t, IOError>) {}

    StateT m_state;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<typename StateT>
struct Http2SslReadMachine {
    using state_result_type = typename StateT::ResultType;
    using result_type = Http2SslResultT<state_result_type>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit Http2SslReadMachine(StateT state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_state.has_result()) {
            return galay::ssl::SslMachineAction<result_type>::complete(
                to_ssl_http2_result(m_state.take_result()));
        }
        if (m_state.parse_from_ring_buffer()) {
            return galay::ssl::SslMachineAction<result_type>::complete(
                to_ssl_http2_result(m_state.take_result()));
        }
        if (m_state.complete_if_closing()) {
            return galay::ssl::SslMachineAction<result_type>::complete(
                to_ssl_http2_result(m_state.take_result()));
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_state.prepare_recv_window(recv_buffer, recv_length)) {
            return galay::ssl::SslMachineAction<result_type>::complete(
                to_ssl_http2_result(m_state.take_result()));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void on_handshake(std::expected<void, galay::ssl::SslError>) {}

    void on_recv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_state.set_ssl_recv_error(result.error());
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_state.set_protocol_error(Http2ErrorCode::ProtocolError, "peer closed");
            return;
        }

        m_state.on_bytes_received(recv_bytes);
    }

    void on_send(std::expected<size_t, galay::ssl::SslError>) {}

    void on_shutdown(std::expected<void, galay::ssl::SslError>) {}

    StateT m_state;
};
#endif

template<typename ResultT, typename InnerOperationT>
class BufferedFastPathOperation
    : public SequenceAwaitableBase
    , public TimeoutMethods<BufferedFastPathOperation<ResultT, InnerOperationT>>
{
public:
    BufferedFastPathOperation(IOController* controller, ResultT ready_result)
        : SequenceAwaitableBase(controller)
        , m_ready_result(std::move(ready_result)) {}

    explicit BufferedFastPathOperation(InnerOperationT inner)
        : SequenceAwaitableBase(inner.m_controller)
        , m_inner_operation(std::move(inner)) {}

    bool await_ready() {
        return m_ready_result.has_value() ||
               (m_inner_operation.has_value() && m_inner_operation->await_ready());
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        if (m_ready_result.has_value()) {
            cancel_bound_timeout_timer();
            return false;
        }
        if (!m_inner_operation.has_value()) {
            cancel_bound_timeout_timer();
            return false;
        }
        forward_bound_timeout_timer(*m_inner_operation);
        return m_inner_operation->await_suspend(handle);
    }

    /**
     * @brief 暂存外层 timeout 绑定，并在 await_suspend() 中转交给 inner。
     * @param timer 定时器
     * @return 无返回值
     */
    void bind_timeout_timer(TimeoutTimer* timer) noexcept {
        SequenceAwaitableBase::bind_timeout_timer(timer);
    }

    ResultT await_resume() {
        if (m_ready_result.has_value()) {
            return std::move(*m_ready_result);
        }
        auto inner_result = m_inner_operation->await_resume();
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(inner_result)>, ResultT>) {
            return inner_result;
        } else {
            return to_outer_http2_result<ResultT>(std::move(inner_result));
        }
    }

    IOTask* front() override {
        return m_inner_operation.has_value() ? m_inner_operation->front() : nullptr;
    }

    const IOTask* front() const override {
        return m_inner_operation.has_value() ? m_inner_operation->front() : nullptr;
    }

    void pop_front() override {
        if (m_inner_operation.has_value()) {
            m_inner_operation->pop_front();
        }
    }

    bool empty() const override {
        return !m_inner_operation.has_value() || m_inner_operation->empty();
    }

#ifdef USE_IOURING
    SequenceProgress prepare_for_submit() override {
        return m_inner_operation.has_value()
            ? m_inner_operation->prepare_for_submit()
            : SequenceProgress::kCompleted;
    }

    SequenceProgress on_active_event(struct io_uring_cqe* cqe, GHandle handle) override {
        return m_inner_operation.has_value()
            ? m_inner_operation->on_active_event(cqe, handle)
            : SequenceProgress::kCompleted;
    }
#else
    SequenceProgress prepare_for_submit(GHandle handle) override {
        return m_inner_operation.has_value()
            ? m_inner_operation->prepare_for_submit(handle)
            : SequenceProgress::kCompleted;
    }

    SequenceProgress on_active_event(GHandle handle) override {
        return m_inner_operation.has_value()
            ? m_inner_operation->on_active_event(handle)
            : SequenceProgress::kCompleted;
    }
#endif

private:
    std::optional<ResultT> m_ready_result;
    std::optional<InnerOperationT> m_inner_operation;
};

struct Http2WriteState {
    using ResultType = std::expected<bool, Http2ErrorCode>;

    explicit Http2WriteState(std::string data)
        : m_data(std::move(data)) {
        if (m_data.empty()) {
            m_result = true;
        }
    }

    bool has_result() const { return m_result.has_value(); }

    ResultType take_result() { return std::move(*m_result); }

    const char* buffer_data() const {
        return remaining() == 0 ? nullptr : m_data.data() + m_offset;
    }

    size_t remaining() const {
        return m_offset >= m_data.size() ? 0 : m_data.size() - m_offset;
    }

    void on_bytes_sent(size_t sent) {
        const size_t left = remaining();
        if (sent > left) {
            m_result = std::unexpected(Http2ErrorCode::InternalError);
            return;
        }

        m_offset += sent;
        if (m_offset >= m_data.size()) {
            m_result = true;
        }
    }

    void set_send_error(const IOError& io_error) {
        m_result = std::unexpected(Http2ErrorCode::InternalError);
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    void set_ssl_send_error(const galay::ssl::SslError& error) {
        const Http2Error http2_error(error);
        m_result = std::unexpected(http2_error.code());
    }
#endif

    std::string m_data;
    size_t m_offset = 0;
    std::optional<ResultType> m_result;
};

struct Http2TcpWriteMachine {
    using result_type = Http2WriteState::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    explicit Http2TcpWriteMachine(Http2WriteState state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_state.has_result()) {
            return MachineAction<result_type>::complete(m_state.take_result());
        }
        return MachineAction<result_type>::wait_write(m_state.buffer_data(), m_state.remaining());
    }

    void on_read(std::expected<size_t, IOError>) {}

    void on_write(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state.set_send_error(result.error());
            return;
        }
        m_state.on_bytes_sent(result.value());
    }

    Http2WriteState m_state;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
struct Http2SslWriteMachine {
    using state_result_type = Http2WriteState::ResultType;
    using result_type = Http2SslResultT<state_result_type>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    explicit Http2SslWriteMachine(Http2WriteState state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_state.has_result()) {
            return galay::ssl::SslMachineAction<result_type>::complete(
                to_ssl_http2_result(m_state.take_result()));
        }
        return galay::ssl::SslMachineAction<result_type>::send(
            m_state.buffer_data(),
            m_state.remaining());
    }

    void on_handshake(std::expected<void, galay::ssl::SslError>) {}

    void on_recv(std::expected<Bytes, galay::ssl::SslError>) {}

    void on_send(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_state.set_ssl_send_error(result.error());
            return;
        }
        m_state.on_bytes_sent(result.value());
    }

    void on_shutdown(std::expected<void, galay::ssl::SslError>) {}

    Http2WriteState m_state;
};
#endif

template<typename SocketType, typename StateT>
auto build_state_machine_read_operation(SocketType& socket, StateT state) {
    using ResultType = typename StateT::ResultType;
    if constexpr (is_ssl_socket_v<SocketType>) {
#ifdef GALAY_SSL_FEATURE_ENABLED
        using SslResultType = Http2SslResultT<ResultType>;
        return galay::ssl::SslAwaitableBuilder<SslResultType>::from_state_machine(
                   socket.controller(),
                   &socket,
                   Http2SslReadMachine<StateT>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::from_state_machine(
                   socket.controller(),
                   Http2TcpReadMachine<StateT>(std::move(state)))
            .build();
    }
}

template<typename SocketType, typename StateT>
using Http2ReadInnerOperationType =
    decltype(build_state_machine_read_operation(std::declval<SocketType&>(), std::declval<StateT>()));

template<typename SocketType, typename StateT>
auto build_read_operation(SocketType& socket, StateT state) {
    using ResultType = typename StateT::ResultType;
    using InnerOperationT = Http2ReadInnerOperationType<SocketType, StateT>;

    if (state.parse_from_ring_buffer() || state.complete_if_closing()) {
        return BufferedFastPathOperation<ResultType, InnerOperationT>(
            socket.controller(),
            state.take_result());
    }

    return BufferedFastPathOperation<ResultType, InnerOperationT>(
        build_state_machine_read_operation(socket, std::move(state)));
}

template<typename SocketType>
auto build_write_state_operation(SocketType& socket, Http2WriteState state) {
    using ResultType = Http2WriteState::ResultType;
    if constexpr (is_ssl_socket_v<SocketType>) {
#ifdef GALAY_SSL_FEATURE_ENABLED
        using SslResultType = Http2SslResultT<ResultType>;
        return galay::ssl::SslAwaitableBuilder<SslResultType>::from_state_machine(
                   socket.controller(),
                   &socket,
                   Http2SslWriteMachine(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::from_state_machine(
                   socket.controller(),
                   Http2TcpWriteMachine(std::move(state)))
            .build();
    }
}

template<typename SocketType>
using Http2WriteInnerOperationType =
    decltype(build_write_state_operation(
        std::declval<SocketType&>(),
        Http2WriteState(std::string{})));

template<typename SocketType>
auto build_write_operation(SocketType& socket, std::string data) {
    using ResultType = Http2WriteState::ResultType;
    using InnerOperationT = Http2WriteInnerOperationType<SocketType>;

    Http2WriteState state(std::move(data));
    if (state.has_result()) {
        return BufferedFastPathOperation<ResultType, InnerOperationT>(
            socket.controller(),
            state.take_result());
    }
    return BufferedFastPathOperation<ResultType, InnerOperationT>(
        build_write_state_operation(socket, std::move(state)));
}

template<typename SocketType>
auto build_write_failure_operation(SocketType& socket, Http2ErrorCode error) {
    using ResultType = Http2WriteState::ResultType;
    using InnerOperationT = Http2WriteInnerOperationType<SocketType>;

    return BufferedFastPathOperation<ResultType, InnerOperationT>(
        socket.controller(),
        ResultType(std::unexpected(error)));
}

template<typename SocketType>
using Http2WriteOperationType =
    decltype(build_write_operation(std::declval<SocketType&>(), std::string{}));

} // namespace detail


/**
 * @brief HTTP/2 连接模板类
 */
template<typename SocketType, RingBufferBackendStrategy Strategy>
class Http2ConnImpl
{
public:
    /**
     * @brief 从 Socket 构造（Prior Knowledge 模式）
     * @param socket 底层 socket
     */
    Http2ConnImpl(SocketType&& socket)
        : m_socket(std::move(socket))
        , m_ring_buffer(65536)  // 64KB buffer
        , m_last_peer_stream_id(0)
        , m_last_local_stream_id(0)
        , m_conn_send_window(kDefaultInitialWindowSize)
        , m_conn_recv_window(kDefaultInitialWindowSize)
        , m_goaway_sent(false)
        , m_goaway_received(false)
        , m_peer_closed(false)
        , m_closing(false)
        , m_expecting_continuation(false)
        , m_continuation_stream_id(0)
        , m_is_client(false)
    {
    }

    /**
     * @brief 从 HttpConn 升级构造（h2c Upgrade 模式）
     * @param http_conn 用于升级的 HTTP 连接
     * @details 类似 WebSocket 从 HTTP/1.1 升级的方式
     */
    Http2ConnImpl(galay::http::HttpConnImpl<SocketType>&& http_conn)
        : m_socket(std::move(http_conn.m_socket))
        , m_ring_buffer(std::move(http_conn.m_ring_buffer))
        , m_last_peer_stream_id(0)
        , m_last_local_stream_id(0)
        , m_conn_send_window(kDefaultInitialWindowSize)
        , m_conn_recv_window(kDefaultInitialWindowSize)
        , m_goaway_sent(false)
        , m_goaway_received(false)
        , m_peer_closed(false)
        , m_closing(false)
        , m_expecting_continuation(false)
        , m_continuation_stream_id(0)
        , m_is_client(false)
    {
        // 升级后需要扩展 buffer 大小以适应 HTTP/2
        if (m_ring_buffer.capacity() < 65536) {
            // 保留已有数据，扩展容量
            RingBuffer<Strategy, std::dynamic_extent> new_buffer(65536);
            // 复制已有数据到新 buffer
            auto read_iovecs = borrow_read_iovecs(m_ring_buffer);
            for (const auto& iov : read_iovecs) {
                size_t remaining = iov.iov_len;
                const char* src = static_cast<const char*>(iov.iov_base);
                while (remaining > 0) {
                    auto write_iovecs = borrow_write_iovecs(new_buffer);
                    if (write_iovecs.empty()) break;
                    for (const auto& wv : write_iovecs) {
                        if (remaining == 0) break;
                        size_t to_copy = std::min(remaining, wv.iov_len);
                        std::memcpy(wv.iov_base, src, to_copy);
                        new_buffer.produce(to_copy);
                        src += to_copy;
                        remaining -= to_copy;
                    }
                }
            }
            m_ring_buffer = std::move(new_buffer);
        }
    }

    /**
     * @brief 从 Socket 和 RingBuffer 构造
     * @param socket 底层 socket
     * @param ring_buffer 环形缓冲区
     */
    Http2ConnImpl(SocketType&& socket, RingBuffer<Strategy, std::dynamic_extent>&& ring_buffer)
        : m_socket(std::move(socket))
        , m_ring_buffer(std::move(ring_buffer))
        , m_last_peer_stream_id(0)
        , m_last_local_stream_id(0)
        , m_conn_send_window(kDefaultInitialWindowSize)
        , m_conn_recv_window(kDefaultInitialWindowSize)
        , m_goaway_sent(false)
        , m_goaway_received(false)
        , m_peer_closed(false)
        , m_closing(false)
        , m_expecting_continuation(false)
        , m_continuation_stream_id(0)
        , m_is_client(false)
    {
    }

    ~Http2ConnImpl();

    // 禁用拷贝
    Http2ConnImpl(const Http2ConnImpl&) = delete;
    Http2ConnImpl& operator=(const Http2ConnImpl&) = delete;

    // 启用移动
    Http2ConnImpl(Http2ConnImpl&&) noexcept;
    Http2ConnImpl& operator=(Http2ConnImpl&&) noexcept;
    
    // 获取 socket
    SocketType& socket() { return m_socket; }
    
    // 获取本地/对端设置
    Http2Settings& local_settings() { return m_local_settings; }
    Http2Settings& peer_settings() { return m_peer_settings; }
    Http2RuntimeConfig& runtime_config() { return m_runtime_config; }
    const Http2RuntimeConfig& runtime_config() const { return m_runtime_config; }

    /**
     * @brief 校验 SETTINGS 帧的连接级约束。
     * @param frame 待处理的 SETTINGS 帧，必须位于 stream 0。
     * @return 非 0 stream 返回 ProtocolError；ACK 携带负载返回 FrameSizeError。
     */
    static Http2ErrorCode validate_settings_frame(const Http2SettingsFrame& frame) {
        if (frame.stream_id() != 0) {
            return Http2ErrorCode::ProtocolError;
        }
        if (frame.is_ack() && !frame.settings().empty()) {
            return Http2ErrorCode::FrameSizeError;
        }
        return Http2ErrorCode::NoError;
    }

    /**
     * @brief 归一化本地 SETTINGS 配置，避免序列化非法值。
     * @param config 待归一化的配置副本。
     * @return 满足 HTTP/2 SETTINGS 取值范围的配置副本。
     */
    template<typename Config>
    static Config normalize_settings_config(Config config) {
        if constexpr (requires { config.initial_window_size; }) {
            if (config.initial_window_size > 2147483647u) {
                config.initial_window_size = 2147483647u;
            }
        }
        if constexpr (requires { config.max_frame_size; }) {
            if (config.max_frame_size < kMinFrameSize) {
                config.max_frame_size = kMinFrameSize;
            } else if (config.max_frame_size > kMaxFrameSize) {
                config.max_frame_size = kMaxFrameSize;
            }
        }
        return config;
    }

    /**
     * @brief 从配置构造已归一化的 SETTINGS 帧。
     * @param config 本地配置。
     * @param enable_push_override 可选的 ENABLE_PUSH 覆盖值。
     * @return 可直接发送或应用的 SETTINGS 帧。
     */
    template<typename Config>
    static Http2SettingsFrame make_settings_frame_from_config(
        Config config,
        std::optional<uint32_t> enable_push_override = std::nullopt) {
        auto normalized = normalize_settings_config(std::move(config));
        Http2Settings settings;
        settings.from(normalized);
        if (enable_push_override.has_value()) {
            settings.enable_push = *enable_push_override;
        }
        return settings.to_frame();
    }

    /**
     * @brief 解码 h2c Upgrade 请求中的 HTTP2-Settings 头。
     * @param header_value HTTP2-Settings 头值，格式为 base64url 编码的 SETTINGS payload。
     * @return 成功时返回 SETTINGS 帧，失败时返回对应 HTTP/2 错误码。
     */
    static std::expected<Http2SettingsFrame, Http2ErrorCode>
    decode_h2c_upgrade_settings_header(std::string_view header_value) {
        std::string base64_value(header_value);
        for (char& ch : base64_value) {
            if (ch == '-') {
                ch = '+';
            } else if (ch == '_') {
                ch = '/';
            }
        }
        switch (base64_value.size() % 4) {
            case 0:
                break;
            case 2:
                base64_value.append("==");
                break;
            case 3:
                base64_value.push_back('=');
                break;
            default:
                return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        if (!galay::utils::Base64Util::base64_can_decode(base64_value)) {
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }
        std::string payload = galay::utils::Base64Util::base64_decode(base64_value);
        if (payload.size() % 6 != 0) {
            return std::unexpected(Http2ErrorCode::ProtocolError);
        }

        Http2SettingsFrame frame;
        frame.header().stream_id = 0;
        for (size_t offset = 0; offset < payload.size(); offset += 6) {
            const auto* bytes =
                reinterpret_cast<const uint8_t*>(payload.data() + offset);
            const auto id = static_cast<Http2SettingsId>(
                (static_cast<uint16_t>(bytes[0]) << 8) |
                static_cast<uint16_t>(bytes[1]));
            const uint32_t value =
                (static_cast<uint32_t>(bytes[2]) << 24) |
                (static_cast<uint32_t>(bytes[3]) << 16) |
                (static_cast<uint32_t>(bytes[4]) << 8) |
                static_cast<uint32_t>(bytes[5]);
            frame.add_setting(id, value);
        }

        Http2Settings validator;
        auto error = validator.apply_settings(frame);
        if (error != Http2ErrorCode::NoError) {
            return std::unexpected(error);
        }
        return frame;
    }

    /**
     * @brief 应用本地 SETTINGS 并同步接收侧 HPACK 限制。
     * @param frame 本端公布的 SETTINGS 帧；ACK 帧为无副作用成功。
     * @return SETTINGS 值非法时返回对应 HTTP/2 错误码。
     */
    Http2ErrorCode apply_local_settings(const Http2SettingsFrame& frame) {
        auto frame_error = validate_settings_frame(frame);
        if (frame_error != Http2ErrorCode::NoError) {
            return frame_error;
        }
        if (frame.is_ack()) {
            return Http2ErrorCode::NoError;
        }

        auto settings_error = m_local_settings.apply_settings(frame);
        if (settings_error != Http2ErrorCode::NoError) {
            return settings_error;
        }
        m_decoder.set_max_table_size(m_local_settings.header_table_size);
        m_decoder.set_max_header_list_size(m_local_settings.max_header_list_size);
        return Http2ErrorCode::NoError;
    }

    /**
     * @brief 应用对端 SETTINGS 并同步发送侧 HPACK 限制。
     * @param frame 对端发送的 SETTINGS 帧；ACK 帧为无副作用成功。
     * @return SETTINGS 值非法时返回对应 HTTP/2 错误码。
     */
    Http2ErrorCode apply_peer_settings(const Http2SettingsFrame& frame) {
        auto frame_error = validate_settings_frame(frame);
        if (frame_error != Http2ErrorCode::NoError) {
            return frame_error;
        }
        if (frame.is_ack()) {
            return Http2ErrorCode::NoError;
        }

        auto settings_error = m_peer_settings.apply_settings(frame);
        if (settings_error != Http2ErrorCode::NoError) {
            return settings_error;
        }
        m_encoder.set_max_table_size(m_peer_settings.header_table_size);
        return Http2ErrorCode::NoError;
    }
    
    // HPACK 编解码器
    HpackEncoder& encoder() { return m_encoder; }
    HpackDecoder& decoder() { return m_decoder; }
    
    // 流管理
    Http2Stream::ptr get_stream(uint32_t stream_id) {
        auto it = m_streams.find(stream_id);
        return it != m_streams.end() ? it->second : nullptr;
    }
    
    Http2Stream::ptr create_stream(uint32_t stream_id, Http2Stream::ptr stream = nullptr) {
        auto [it, inserted] = m_streams.try_emplace(stream_id);
        if (inserted || !it->second) {
            it->second = stream ? std::move(stream) : Http2Stream::create(stream_id);
            it->second->m_send_window = static_cast<int32_t>(m_peer_settings.initial_window_size);
            it->second->m_recv_window = static_cast<int32_t>(m_local_settings.initial_window_size);
        }
        return it->second;
    }
    
    void remove_stream(uint32_t stream_id) {
        m_streams.erase(stream_id);
    }

    void reserve_streams(size_t capacity) {
        if (capacity == 0) {
            return;
        }
        if (capacity > m_streams.bucket_count()) {
            m_streams.reserve(capacity);
        }
    }
    
    size_t stream_count() const { return m_streams.size(); }

    // 遍历所有流
    template<typename Func>
    void for_each_stream(Func&& func) {
        for (auto& [id, stream] : m_streams) {
            func(id, stream);
        }
    }

    // 获取下一个本地流 ID（服务器使用偶数）
    uint32_t next_local_stream_id() {
        if (m_last_local_stream_id == 0) {
            m_last_local_stream_id = 2;
        } else {
            m_last_local_stream_id += 2;
        }
        return m_last_local_stream_id;
    }
    
    // 连接级流量控制
    int32_t conn_send_window() const { return m_conn_send_window; }
    int32_t conn_recv_window() const { return m_conn_recv_window; }
    void adjust_conn_send_window(int32_t delta) { m_conn_send_window += delta; }
    void adjust_conn_recv_window(int32_t delta) { m_conn_recv_window += delta; }
    Http2FlowControlUpdate evaluate_recv_window_update(int32_t stream_recv_window, size_t data_size) const {
        uint32_t conn_target = m_runtime_config.flow_control_target_window == 0
            ? m_local_settings.initial_window_size
            : m_runtime_config.flow_control_target_window;
        if (conn_target == 0) {
            conn_target = kDefaultInitialWindowSize;
        }
        uint32_t stream_target = m_local_settings.initial_window_size == 0
            ? kDefaultInitialWindowSize
            : m_local_settings.initial_window_size;

        if (m_runtime_config.flow_control_strategy) {
            return m_runtime_config.flow_control_strategy(
                m_conn_recv_window, stream_recv_window, conn_target, data_size);
        }

        Http2FlowControlUpdate update;
        const int32_t conn_low_watermark = static_cast<int32_t>((conn_target * 3) / 4);
        const int32_t stream_low_watermark = static_cast<int32_t>((stream_target * 3) / 4);
        if (m_conn_recv_window < conn_low_watermark) {
            update.conn_increment = static_cast<uint32_t>(conn_target - m_conn_recv_window);
        }
        if (stream_recv_window < stream_low_watermark) {
            update.stream_increment = static_cast<uint32_t>(stream_target - stream_recv_window);
        }
        return update;
    }
    
    // 客户端/服务端模式
    bool is_client() const { return m_is_client; }
    void set_is_client(bool is_client) { m_is_client = is_client; }

    // GOAWAY 状态
    bool is_goaway_sent() const { return m_goaway_sent; }
    bool is_goaway_received() const { return m_goaway_received; }
    void set_goaway_sent() { m_goaway_sent = true; }
    void set_goaway_received() { m_goaway_received = true; }
    void mark_goaway_received(uint32_t last_stream_id,
                            Http2ErrorCode error_code,
                            std::string debug = "") {
        m_goaway_received = true;
        m_draining = true;
        m_goaway_last_stream_id = last_stream_id;
        m_goaway_error_code = error_code;
        m_goaway_debug_data = std::move(debug);
    }
    void mark_goaway_sent(uint32_t last_stream_id,
                        Http2ErrorCode error_code,
                        std::string debug = "") {
        m_goaway_sent = true;
        m_draining = true;
        m_goaway_last_stream_id = last_stream_id;
        m_goaway_error_code = error_code;
        m_goaway_debug_data = std::move(debug);
    }
    bool is_draining() const { return m_draining; }
    void set_draining(bool draining) { m_draining = draining; }
    uint32_t goaway_last_stream_id() const { return m_goaway_last_stream_id; }
    Http2ErrorCode goaway_error_code() const { return m_goaway_error_code; }
    const std::string& goaway_debug_data() const { return m_goaway_debug_data; }

    void mark_settings_sent() {
        m_settings_ack_pending = true;
        m_settings_sent_at = std::chrono::steady_clock::now();
    }
    void mark_settings_ack_received() { m_settings_ack_pending = false; }
    bool is_settings_ack_pending() const { return m_settings_ack_pending; }
    std::chrono::steady_clock::time_point settings_sent_at() const { return m_settings_sent_at; }

    bool is_peer_closed() const { return m_peer_closed; }
    bool is_closing() const { return m_closing; }
    const std::string& last_read_error() const { return m_last_read_error; }
    void clear_last_read_error() { m_last_read_error.clear(); }
    void set_last_read_error(std::string message) { m_last_read_error = std::move(message); }
    void mark_peer_closed(std::string message = "peer closed") {
        m_peer_closed = true;
        m_last_read_error = std::move(message);
    }
    
    // 最后处理的流 ID
    uint32_t last_peer_stream_id() const { return m_last_peer_stream_id; }
    void set_last_peer_stream_id(uint32_t id) { m_last_peer_stream_id = id; }
    
    // CONTINUATION 状态
    bool is_expecting_continuation() const { return m_expecting_continuation; }
    uint32_t continuation_stream_id() const { return m_continuation_stream_id; }
    void set_expecting_continuation(bool expecting, uint32_t stream_id = 0) {
        m_expecting_continuation = expecting;
        m_continuation_stream_id = stream_id;
    }
    
    // 关闭连接（可 co_await 的 close operation）。
    // 只负责传输层 teardown；协议级清理由 StreamManager 负责。
    auto close() {
        m_closing = true;
        // shutdown(fd) 触发读事件（readv 返回 0），让 reader_loop 退出阻塞读取。
        const int fd = m_socket.handle().fd;
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
        return m_socket.close();
    }

    // 非 co_await 关闭：仅设置 closing 标志并触发 TCP shutdown，
    // 用于唤醒 reader_loop；不执行协议级收尾。
    void initiate_close() {
        m_closing = true;
        const int fd = m_socket.handle().fd;
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    // StreamManager 访问（需要 include stream_manager.h 后才能使用）
    Http2StreamManagerImpl<SocketType, Strategy>* stream_manager() { return m_stream_manager.get(); }
    void init_stream_manager() {
        if (!m_stream_manager) {
            m_stream_manager = std::make_unique<Http2StreamManagerImpl<SocketType, Strategy>>(*this);
        }
    }

    Http2ConnectionCore* connection_core() { return m_connection_core.get(); }
    Http2ConnectionCore& ensure_connection_core() {
        if (!m_connection_core) {
            m_connection_core = std::make_unique<Http2ConnectionCore>();
        }
        return *m_connection_core;
    }

    /**
     * @brief 获取接收缓冲区引用
     * @return RingBuffer<Strategy, std::dynamic_extent>& 引用
     */
    RingBuffer<Strategy, std::dynamic_extent>& ring_buffer() { return m_ring_buffer; }

    /**
     * @brief 将数据放入接收缓冲区
     * @param data 数据指针
     * @param len 数据长度
     * @return 无返回值
     */
    void feed_data(const char* data, size_t len) {
        auto write_iovecs = borrow_write_iovecs(m_ring_buffer);
        size_t copied = 0;
        for (const auto& iov : write_iovecs) {
            size_t to_copy = std::min(iov.iov_len, len - copied);
            std::memcpy(iov.iov_base, data + copied, to_copy);
            copied += to_copy;
            if (copied >= len) break;
        }
        m_ring_buffer.produce(copied);
    }

    /**
     * @brief 解析缓冲区中已有的完整帧（批量）
     * @param max_count 最多解析的帧数量（默认无限制）
     * @return 成功时返回帧向量，失败时返回错误码
     * @details
     * - 仅解析已缓冲的完整帧，不执行任何 socket recv
     * - 遇到不完整的尾部帧时停止（不报错）
     * - 验证帧头 length <= local_settings().max_frame_size
     * - 返回 FrameSizeError 如果帧过大
     */
    std::expected<std::vector<Http2Frame::uptr>, Http2ErrorCode>
    parse_buffered_frames(size_t max_count = std::numeric_limits<size_t>::max()) {
        std::vector<Http2Frame::uptr> frames;

        while (frames.size() < max_count) {
            // 检查是否有足够的数据读取帧头
            if (m_ring_buffer.readable() < kHttp2FrameHeaderLength) {
                break;  // 不完整的帧头，停止解析
            }

            auto read_iovecs = borrow_read_iovecs(m_ring_buffer);
            if (read_iovecs.empty()) {
                break;
            }

            // 读取帧头
            Http2FrameHeader header;
            if (read_iovecs[0].iov_len >= kHttp2FrameHeaderLength) {
                header = Http2FrameHeader::deserialize(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base));
            } else {
                // 帧头跨越多个 iovec，需要拷贝
                uint8_t header_buf[kHttp2FrameHeaderLength];
                size_t copied = 0;
                for (const auto& iov : read_iovecs) {
                    size_t to_copy = std::min(iov.iov_len, kHttp2FrameHeaderLength - copied);
                    std::memcpy(header_buf + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= kHttp2FrameHeaderLength) break;
                }
                header = Http2FrameHeader::deserialize(header_buf);
            }

            // 验证帧大小
            if (header.length > m_local_settings.max_frame_size) {
                return std::unexpected(Http2ErrorCode::FrameSizeError);
            }

            size_t total_frame_size = kHttp2FrameHeaderLength + header.length;

            // 检查是否有完整的帧
            if (m_ring_buffer.readable() < total_frame_size) {
                break;  // 不完整的帧，停止解析
            }

            // RFC 9113: unknown extension frames are ignored, but their
            // payload bytes must still be consumed to keep the stream aligned.
            if (!detail::is_known_http2_frame_type(header.type)) {
                m_ring_buffer.consume(total_frame_size);
                continue;
            }

            // 解析完整帧
            std::expected<Http2Frame::uptr, Http2ErrorCode> frame_result;

            if (read_iovecs[0].iov_len >= total_frame_size) {
                // 帧在单个 iovec 中，直接解析
                frame_result = Http2FrameParser::parse_frame(
                    static_cast<const uint8_t*>(read_iovecs[0].iov_base), total_frame_size);
            } else {
                // 帧跨越多个 iovec，需要拷贝到临时缓冲区
                if (m_parse_buffer.size() < total_frame_size) {
                    m_parse_buffer.resize(total_frame_size);
                }
                size_t copied = 0;
                for (const auto& iov : read_iovecs) {
                    size_t to_copy = std::min(iov.iov_len, total_frame_size - copied);
                    std::memcpy(m_parse_buffer.data() + copied, iov.iov_base, to_copy);
                    copied += to_copy;
                    if (copied >= total_frame_size) break;
                }
                frame_result = Http2FrameParser::parse_frame(m_parse_buffer.data(), total_frame_size);
            }

            if (!frame_result) {
                return std::unexpected(frame_result.error());
            }

            // 消费已解析的帧数据
            m_ring_buffer.consume(total_frame_size);
            frames.push_back(std::move(*frame_result));
        }

        return frames;
    }

    // ==================== 帧读写（返回可 co_await 的 operation） ====================
    
    /**
     * @brief 获取帧读取 operation
     * @return 单帧读取等待体，通过 co_await 取得帧或读取错误
     */
    auto read_frame() {
        return detail::build_read_operation(
            m_socket,
            detail::Http2SingleFrameReadState(
                m_ring_buffer,
                m_peer_settings,
                &m_peer_closed,
                &m_last_read_error,
                &m_closing));
    }

    /**
     * @brief 获取批量帧读取 operation
     * @param max_frames 最多处理的帧数量
     * @return 批量帧读取等待体，通过 co_await 取得帧集合或读取错误
     */
    auto read_frames_batch(size_t max_frames = std::numeric_limits<size_t>::max()) {
        return detail::build_read_operation(
            m_socket,
            detail::Http2FrameBatchReadState(
                m_ring_buffer,
                m_peer_settings,
                max_frames,
                &m_peer_closed,
                &m_last_read_error,
                &m_closing));
    }

    template<typename S = SocketType>
    requires (!is_ssl_socket_v<S>)
    auto read_frame_views_batch(size_t max_frames = std::numeric_limits<size_t>::max()) {
        return detail::build_read_operation(
            m_socket,
            detail::Http2FrameViewBatchReadState(
                m_ring_buffer,
                m_peer_settings,
                max_frames,
                &m_peer_closed,
                &m_last_read_error,
                &m_closing));
    }

    /**
     * @brief 获取帧写入 operation
     * @param frame 帧对象
     * @return 帧写入等待体，通过 co_await 取得写入结果
     */
    auto write_frame(const Http2Frame& frame) {
        return detail::build_write_operation(m_socket, frame.serialize());
    }
    
    /**
     * @brief 获取原始数据写入 operation
     * @param data 输入数据
     * @return 原始字节写入等待体，通过 co_await 取得写入结果
     */
    auto write_raw(std::string data) {
        return detail::build_write_operation(m_socket, std::move(data));
    }

    // ==================== 便捷方法 ====================
    
    /**
     * @brief 发送 SETTINGS 帧
     * @return SETTINGS 帧写入等待体，通过 co_await 取得写入结果
     */
    auto send_settings() {
        auto frame = m_local_settings.to_frame();
        mark_settings_sent();
        return write_frame(frame);
    }
    
    /**
     * @brief 发送 SETTINGS ACK
     * @return SETTINGS ACK 写入等待体，通过 co_await 取得写入结果
     */
    auto send_settings_ack() {
        Http2SettingsFrame frame;
        frame.set_ack(true);
        return write_frame(frame);
    }
    
    /**
     * @brief 发送 PING
     * @param data 输入数据
     * @param ack 是否为确认帧
     * @return PING 帧写入等待体，通过 co_await 取得写入结果
     */
    auto send_ping(const uint8_t* data, bool ack = false) {
        Http2PingFrame frame;
        frame.set_opaque_data(data);
        frame.set_ack(ack);
        return write_frame(frame);
    }
    
    /**
     * @brief 发送 GOAWAY
     * @param error 错误信息
     * @param debug 调试数据
     * @param last_stream_id 最后处理的流标识符
     * @return GOAWAY 帧写入等待体，通过 co_await 取得写入结果
     */
    auto send_goaway(Http2ErrorCode error,
                    const std::string& debug = "",
                    std::optional<uint32_t> last_stream_id = std::nullopt) {
        Http2GoAwayFrame frame;
        uint32_t last = last_stream_id.value_or(m_last_peer_stream_id);
        frame.set_last_stream_id(last);
        frame.set_error_code(error);
        frame.set_debug_data(debug);
        mark_goaway_sent(last, error, debug);
        return write_frame(frame);
    }
    
    /**
     * @brief 发送 RST_STREAM
     * @param stream_id 流标识符
     * @param error 错误信息
     * @return RST_STREAM 帧写入等待体，通过 co_await 取得写入结果
     */
    auto send_rst_stream(uint32_t stream_id, Http2ErrorCode error) {
        auto bytes = Http2FrameBuilder::rst_stream_bytes(stream_id, error);
        
        auto stream = get_stream(stream_id);
        if (stream) {
            stream->on_rst_stream_sent();
        }
        
        return write_raw(std::move(bytes));
    }
    
    /**
     * @brief 发送 WINDOW_UPDATE
     * @param stream_id 流标识符
     * @param increment 窗口增量，单位为字节
     * @return WINDOW_UPDATE 帧写入等待体，通过 co_await 取得写入结果
     */
    auto send_window_update(uint32_t stream_id, uint32_t increment) {
        Http2WindowUpdateFrame frame;
        frame.header().stream_id = stream_id;
        frame.set_window_size_increment(increment);
        return write_frame(frame);
    }
    
    /**
     * @brief 发送 HEADERS 帧
     * @param stream_id 流标识符
     * @param headers 头部字段集合
     * @param end_stream 是否结束当前流
     * @param end_headers 是否结束头部块
     * @return HEADERS 帧写入等待体，通过 co_await 取得写入结果
     */
    auto send_headers(
        uint32_t stream_id, 
        const std::vector<Http2HeaderField>& headers,
        bool end_stream = false,
        bool end_headers = true)
    {
        std::string header_block = m_encoder.encode(headers);
        auto bytes = Http2FrameBuilder::headers_bytes(stream_id,
                                                     header_block,
                                                     end_stream,
                                                     end_headers);
        
        auto stream = get_stream(stream_id);
        if (stream) {
            stream->on_headers_sent(end_stream);
        }
        
        return write_raw(std::move(bytes));
    }
    
    /**
     * @brief 发送 DATA 帧（单帧）
     * @param stream_id 流标识符
     * @param data 输入数据引用
     * @param end_stream 是否结束当前流
     * @return 单个 DATA 帧写入等待体，通过 co_await 取得写入结果
     * @details 发送窗口不足时返回立即就绪的 FlowControlError，不挂起也不占用 WRITE 槽位。
     */
    auto send_data_frame(
        uint32_t stream_id,
        const std::string& data,
        bool end_stream = false)
    {
        auto stream = get_stream(stream_id);
        const size_t data_size = data.size();
        if (data_size > 0) {
            if (!stream ||
                data_size > static_cast<size_t>(std::max<int32_t>(m_conn_send_window, 0)) ||
                data_size > static_cast<size_t>(std::max<int32_t>(stream->send_window(), 0))) {
                return detail::build_write_failure_operation(
                    m_socket, Http2ErrorCode::FlowControlError);
            }
        }

        auto bytes = Http2FrameBuilder::data_bytes(stream_id, data, end_stream);
        if (stream) {
            const auto delta = static_cast<int32_t>(data_size);
            m_conn_send_window -= delta;
            stream->adjust_send_window(-delta);
            if (end_stream) {
                stream->on_data_sent(true);
            }
        }
        
        return write_raw(std::move(bytes));
    }
    
    /**
     * @brief 发送 PUSH_PROMISE 帧
     * @param stream_id 流标识符
     * @param promised_stream_id 承诺流标识符
     * @param headers 头部字段集合
     * @return PUSH_PROMISE 帧写入等待体，通过 co_await 取得写入结果
     */
    auto send_push_promise(
        uint32_t stream_id,
        uint32_t promised_stream_id,
        const std::vector<Http2HeaderField>& headers)
    {
        std::string header_block = m_encoder.encode(headers);
        
        Http2PushPromiseFrame frame;
        frame.header().stream_id = stream_id;
        frame.set_promised_stream_id(promised_stream_id);
        frame.set_header_block(std::move(header_block));
        frame.set_end_headers(true);
        
        return write_frame(frame);
    }

    struct PushPromisePrepareResult {
        uint32_t promised_stream_id;
        detail::Http2WriteOperationType<SocketType> send_operation;
    };
    
    /**
     * @brief 创建推送流并准备 PUSH_PROMISE
     * @param stream_id 流标识符
     * @param method 方法名称
     * @param path 路径
     * @param authority 请求目标主机与端口
     * @param scheme 请求协议方案
     * @return 推送准备结果；如果推送被禁用返回 nullopt
     */
    std::optional<PushPromisePrepareResult> prepare_push_promise(
        uint32_t stream_id,
        const std::string& method,
        const std::string& path,
        const std::string& authority,
        const std::string& scheme = "http")
    {
        if (!m_peer_settings.enable_push) {
            return std::nullopt;
        }
        
        uint32_t promised_stream_id = next_local_stream_id();
        
        std::vector<Http2HeaderField> headers;
        headers.push_back({":method", method});
        headers.push_back({":path", path});
        headers.push_back({":authority", authority});
        headers.push_back({":scheme", scheme});
        
        // 创建推送流
        auto push_stream = create_stream(promised_stream_id);
        push_stream->set_state(Http2StreamState::ReservedLocal);
        
        return PushPromisePrepareResult{
            promised_stream_id,
            send_push_promise(stream_id, promised_stream_id, headers)
        };
    }

private:
    friend class Http2StreamManagerImpl<SocketType, Strategy>;

    SocketType m_socket;
    RingBuffer<Strategy, std::dynamic_extent> m_ring_buffer;
    std::vector<uint8_t> m_parse_buffer;  // 用于跨 iovec 边界的帧解析

    // 连接设置
    Http2Settings m_local_settings;
    Http2Settings m_peer_settings;
    Http2RuntimeConfig m_runtime_config;
    
    // 流管理
    std::unordered_map<uint32_t, Http2Stream::ptr> m_streams;
    uint32_t m_last_peer_stream_id;
    uint32_t m_last_local_stream_id;
    
    // HPACK 编解码器
    HpackEncoder m_encoder;
    HpackDecoder m_decoder;
    
    // 连接级流量控制
    int32_t m_conn_send_window;
    int32_t m_conn_recv_window;
    
    // 连接状态
    bool m_goaway_sent;
    bool m_goaway_received;
    bool m_draining = false;
    uint32_t m_goaway_last_stream_id = 0;
    Http2ErrorCode m_goaway_error_code = Http2ErrorCode::NoError;
    std::string m_goaway_debug_data;
    bool m_settings_ack_pending = false;
    std::chrono::steady_clock::time_point m_settings_sent_at{};
    bool m_is_client;
    bool m_peer_closed;
    bool m_closing;
    std::string m_last_read_error;

    // CONTINUATION 状态
    bool m_expecting_continuation;
    uint32_t m_continuation_stream_id;

    // StreamManager
    std::unique_ptr<Http2StreamManagerImpl<SocketType, Strategy>> m_stream_manager;
    std::unique_ptr<Http2ConnectionCore> m_connection_core;
};

using Http2Conn = Http2ConnImpl<galay::async::AsyncTcpSocket>;

#ifdef GALAY_SSL_FEATURE_ENABLED
using Http2sConn = Http2ConnImpl<galay::ssl::SslSocket>;
#endif

} // namespace galay::http2

// Http2StreamManager 的完整定义（解决 unique_ptr 析构需要完整类型的问题）
#include "stream_manager.h"

namespace galay::http2
{

template<typename SocketType, RingBufferBackendStrategy Strategy>
Http2ConnImpl<SocketType, Strategy>::~Http2ConnImpl() = default;

template<typename SocketType, RingBufferBackendStrategy Strategy>
Http2ConnImpl<SocketType, Strategy>::Http2ConnImpl(Http2ConnImpl&&) noexcept = default;

template<typename SocketType, RingBufferBackendStrategy Strategy>
Http2ConnImpl<SocketType, Strategy>& Http2ConnImpl<SocketType, Strategy>::operator=(
    Http2ConnImpl&&) noexcept = default;

} // namespace galay::http2

#endif // GALAY_HTTP2_CONN_H
