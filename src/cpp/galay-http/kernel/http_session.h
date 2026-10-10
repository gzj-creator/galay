/**
 * @file http_session.h
 * @brief HTTP 客户端会话，整合请求发送与响应接收
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HttpSessionImpl 模板类，将 HTTP 请求的发送和响应的接收
 * 整合为一个异步操作。支持 GET/POST/PUT/DELETE 等 HTTP 方法，
 * 内部使用状态机驱动发送-接收-解析流程。
 */

#ifndef GALAY_HTTP_SESSION_H
#define GALAY_HTTP_SESSION_H

#include "http_reader.h"
#include "http_writer.h"
#include "../protoc/http_request.h"
#include "../protoc/http_response.h"
#include "../../galay-kernel/async/async_tcp.h"
#include "../../galay-utils/buffer/bytes.hpp"
#include "../../galay-utils/buffer/ring_buffer.hpp"
#include "../../galay-kernel/core/awaitable.h"
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-ssl/async/ssl_await.h"
#include "../../galay-ssl/async/ssl_socket.h"
#endif

namespace galay::http {

using namespace galay::async;
using namespace galay::kernel;
using ::galay::utils::Bytes;
using ::galay::utils::RingBuffer;

template<typename SocketType>
class HttpSessionImpl;

namespace detail {

/**
 * @brief HTTP 会话状态，管理请求发送和响应解析
 * @tparam SocketType Socket 类型
 */
template<typename SocketType>
struct HttpSessionState {
    using ResultType = std::expected<std::optional<HttpResponse>, HttpError>; ///< 结果类型

    /**
     * @brief 从 HttpRequest 对象构造
     * @param session 所属会话
     * @param request 待发送的请求（会自动序列化）
     */
    HttpSessionState(HttpSessionImpl<SocketType>& session, HttpRequest&& request)
        : m_session(&session)
        , m_request(std::move(request))
        , m_send_buffer(m_request.to_string()) {}

    /**
     * @brief 从已序列化的请求字符串构造
     * @param session 所属会话
     * @param serialized_request 完整的 HTTP 请求报文
     */
    HttpSessionState(HttpSessionImpl<SocketType>& session, std::string&& serialized_request)
        : m_session(&session)
        , m_send_buffer(std::move(serialized_request)) {
        const std::string_view request_line(m_send_buffer);
        m_request.header().method() = string_to_http_method(
            request_line.substr(0, request_line.find(' ')));
    }

    bool send_completed() const { ///< 判断请求是否已完全发送
        return m_send_offset >= m_send_buffer.size();
    }

    const char* send_buffer() const { ///< 获取当前发送缓冲区指针
        return m_send_buffer.data() + m_send_offset;
    }

    size_t send_remaining() const { ///< 获取剩余待发送字节数
        return m_send_buffer.size() - m_send_offset;
    }

    void on_bytes_sent(size_t sent) { ///< 处理已发送字节数
        m_send_offset += sent;
    }

    /**
     * @brief 从 RingBuffer 中尝试解析 HTTP 响应
     * @return 解析完成返回 true
     */
    bool parse_from_ring_buffer() {
        auto read_iovecs = borrow_read_iovecs(m_session->get_ring_buffer());
        if (read_iovecs.empty()) {
            return false;
        }

        if (IoVecWindow::build_window(read_iovecs, m_parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] =
            m_response.from_io_vec(m_parse_iovecs, m_session->get_reader_setting().get_max_body_size(),
                                   m_request.header().method());
        if (consumed > 0) {
            m_session->get_ring_buffer().consume(static_cast<size_t>(consumed));
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_session->get_reader_setting().get_max_header_size() &&
                !m_response.is_complete()) {
                set_parse_error(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            set_parse_error(HttpError(error_code));
            return true;
        }

        if (!m_response.is_complete()) {
            return false;
        }

        m_response_value = std::optional<HttpResponse>(std::move(m_response));
        return true;
    }

    bool prepare_recv_window() { ///< 准备 TCP 接收窗口
        m_write_iovecs = borrow_write_iovecs(m_session->get_ring_buffer());
        if (m_write_iovecs.empty()) {
            set_parse_error(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    bool prepare_recv_window(char*& buffer, size_t& length) { ///< 准备 SSL 接收窗口
        if (!prepare_recv_window()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bind_first_non_empty(m_write_iovecs, buffer, length)) {
            set_parse_error(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    const struct iovec* recv_iovecs_data() const { return m_write_iovecs.data(); }
    size_t recv_iovecs_count() const { return m_write_iovecs.size(); }

    void set_send_error(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kTimeout)) {
            m_error = HttpError(kRequestTimeOut, io_error.message());
            return;
        }
        m_error = HttpError(kSendError, io_error.message());
    }

    void set_recv_error(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kTimeout)) {
            m_error = HttpError(kRequestTimeOut, io_error.message());
            return;
        }
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_error = HttpError(kConnectionClose);
            return;
        }
        m_error = HttpError(kTcpRecvError, io_error.message());
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    void set_ssl_send_error(const galay::ssl::SslError& error) {
        m_error = HttpError(error);
    }

    void set_ssl_recv_error(const galay::ssl::SslError& error) {
        m_error = HttpError(error);
    }
#endif

    void on_peer_closed() {
        m_error = HttpError(kConnectionClose);
    }

    void on_bytes_received(size_t recv_bytes) {
        m_session->get_ring_buffer().produce(recv_bytes);
        m_total_received += recv_bytes;
    }

    void set_parse_error(HttpError&& error) {
        m_error = std::move(error);
    }

    ResultType take_result() {
        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }
        if (m_response_value.has_value()) {
            return std::move(*m_response_value);
        }
        return std::optional<HttpResponse>{};
    }

    HttpSessionImpl<SocketType>* m_session;                             ///< 所属会话指针
    HttpRequest m_request;                                              ///< 待发送的请求
    HttpResponse m_response;                                            ///< 接收到的响应
    std::string m_send_buffer;                                          ///< 发送缓冲区
    size_t m_send_offset = 0;                                           ///< 已发送偏移量
    size_t m_total_received = 0;                                        ///< 已接收总字节数
    std::vector<iovec> m_parse_iovecs;                                  ///< 解析用 iovec 缓冲
    BorrowedIovecs<2> m_write_iovecs;                                   ///< 接收窗口 iovec
    std::optional<HttpError> m_error;                                   ///< HTTP 错误
    std::optional<std::optional<HttpResponse>> m_response_value;        ///< 解析完成的响应值
};

/**
 * @brief HTTP 会话 TCP 状态机
 * @details 驱动请求发送（write）和响应接收（readv）的异步流程
 */
template<typename SocketType>
struct HttpSessionTcpMachine {
    using result_type = typename HttpSessionState<SocketType>::ResultType;

    explicit HttpSessionTcpMachine(std::shared_ptr<HttpSessionState<SocketType>> state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->send_completed()) {
            return MachineAction<result_type>::wait_write(
                m_state->send_buffer(),
                m_state->send_remaining());
        }

        if (m_state->parse_from_ring_buffer()) {
            m_result = m_state->take_result();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepare_recv_window()) {
            m_result = m_state->take_result();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::wait_readv(
            m_state->recv_iovecs_data(),
            m_state->recv_iovecs_count());
    }

    void on_read(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->set_recv_error(result.error());
            m_result = m_state->take_result();
            return;
        }

        if (result.value() == 0) {
            m_state->on_peer_closed();
            m_result = m_state->take_result();
            return;
        }

        m_state->on_bytes_received(result.value());
    }

    void on_write(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->set_send_error(result.error());
            m_result = m_state->take_result();
            return;
        }

        m_state->on_bytes_sent(result.value());
    }

    std::shared_ptr<HttpSessionState<SocketType>> m_state;
    std::optional<result_type> m_result;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
/**
 * @brief HTTP 会话 SSL 状态机
 * @details SSL 版本的会话状态机，使用 SSL send/recv 驱动
 */
template<typename SocketType>
struct HttpSessionSslMachine {
    using result_type = typename HttpSessionState<SocketType>::ResultType;

    explicit HttpSessionSslMachine(std::shared_ptr<HttpSessionState<SocketType>> state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->send_completed()) {
            return galay::ssl::SslMachineAction<result_type>::send(
                m_state->send_buffer(),
                m_state->send_remaining());
        }

        if (m_state->parse_from_ring_buffer()) {
            m_result = m_state->take_result();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_state->prepare_recv_window(recv_buffer, recv_length)) {
            m_result = m_state->take_result();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void on_handshake(std::expected<void, galay::ssl::SslError>) {}

    void on_recv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_state->set_ssl_recv_error(result.error());
            m_result = m_state->take_result();
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_state->on_peer_closed();
            m_result = m_state->take_result();
            return;
        }

        m_state->on_bytes_received(recv_bytes);
    }

    void on_send(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_state->set_ssl_send_error(result.error());
            m_result = m_state->take_result();
            return;
        }

        m_state->on_bytes_sent(result.value());
    }

    void on_shutdown(std::expected<void, galay::ssl::SslError>) {}

    std::shared_ptr<HttpSessionState<SocketType>> m_state;
    std::optional<result_type> m_result;
};
#endif

/**
 * @brief 构建 HTTP 会话异步操作（从 HttpRequest）
 * @tparam SocketType Socket 类型
 * @param session HTTP 会话引用
 * @param request 待发送的请求
 * @return 可 co_await 的异步操作对象
 */
template<typename SocketType>
auto build_session_operation(HttpSessionImpl<SocketType>& session, HttpRequest&& request) {
    using State = HttpSessionState<SocketType>;
    using ResultType = typename State::ResultType;
    auto state = std::make_shared<State>(session, std::move(request));

    if constexpr (std::is_same_v<SocketType, AsyncTcpSocket>) {
        return AwaitableBuilder<ResultType>::from_state_machine(
                   session.get_socket().controller(),
                   HttpSessionTcpMachine<SocketType>(std::move(state)))
            .build();
    } else {
#ifdef GALAY_SSL_FEATURE_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::from_state_machine(
                   session.get_socket().controller(),
                   &session.get_socket(),
                   HttpSessionSslMachine<SocketType>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    }
}

/**
 * @brief 构建 HTTP 会话异步操作（从已序列化的请求字符串）
 * @tparam SocketType Socket 类型
 * @param session HTTP 会话引用
 * @param serialized_request 完整的 HTTP 请求报文
 * @return 可 co_await 的异步操作对象
 */
template<typename SocketType>
auto build_session_operation(HttpSessionImpl<SocketType>& session, std::string&& serialized_request) {
    using State = HttpSessionState<SocketType>;
    using ResultType = typename State::ResultType;
    auto state = std::make_shared<State>(session, std::move(serialized_request));

    if constexpr (std::is_same_v<SocketType, AsyncTcpSocket>) {
        return AwaitableBuilder<ResultType>::from_state_machine(
                   session.get_socket().controller(),
                   HttpSessionTcpMachine<SocketType>(std::move(state)))
            .build();
    } else {
#ifdef GALAY_SSL_FEATURE_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::from_state_machine(
                   session.get_socket().controller(),
                   &session.get_socket(),
                   HttpSessionSslMachine<SocketType>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    }
}

} // namespace detail

/**
 * @brief HTTP 客户端会话模板类
 * @tparam SocketType Socket 类型（AsyncTcpSocket 或 SslSocket）
 * @details 整合 HTTP 请求发送与响应接收，提供便捷的 HTTP 方法调用接口。
 *          内部使用状态机驱动发送-接收-解析流程，支持连接复用。
 */
template<typename SocketType>
class HttpSessionImpl {
public:
    /**
     * @brief 构造函数
     * @param socket Socket 引用
     * @param ring_buffer_size RingBuffer 大小
     * @param reader_setting 读取器配置
     * @param writer_setting 写入器配置
     */
    HttpSessionImpl(SocketType& socket,
                    size_t ring_buffer_size = 8192,
                    const HttpReaderSetting& reader_setting = HttpReaderSetting(),
                    const HttpWriterSetting& writer_setting = HttpWriterSetting())
        : m_socket(socket)
        , m_ring_buffer(ring_buffer_size)
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_reader(m_ring_buffer, m_reader_setting, socket)
        , m_writer(m_writer_setting, socket) {}

    /**
     * @brief 获取读取器引用
     * @return HttpReaderImpl<SocketType>& 引用
     */
    HttpReaderImpl<SocketType>& get_reader() { return m_reader; }
    /**
     * @brief 获取写入器引用
     * @return HttpWriterImpl<SocketType>& 引用
     */
    HttpWriterImpl<SocketType>& get_writer() { return m_writer; }
    /**
     * @brief 获取底层 Socket 引用
     * @return SocketType& 引用
     */
    SocketType& get_socket() { return m_socket; }
    /**
     * @brief 获取 RingBuffer 引用
     * @return RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent>& 引用
     */
    RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent>& get_ring_buffer() { return m_ring_buffer; }
    /**
     * @brief 获取读取器配置
     * @return const HttpReaderSetting& 引用
     */
    const HttpReaderSetting& get_reader_setting() const { return m_reader_setting; }

    /**
     * @brief 发送 GET 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto get(const std::string& uri,
             const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::GET, uri, "", "", headers);
    }

    /**
     * @brief 发送 POST 请求
     * @param uri 请求 URI
     * @param body 请求体
     * @param content_type Content-Type
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto post(const std::string& uri,
              const std::string& body,
              const std::string& content_type = "application/x-www-form-urlencoded",
              const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::POST, uri, body, content_type, headers);
    }

    /**
     * @brief 发送带右值请求体的 POST 请求
     * @param uri 请求 URI
     * @param body 调用方可转移所有权的请求体
     * @param content_type 请求体 Content-Type
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     * @note 该重载会把请求体直接移动进内部 HttpRequest，适合热点路径避免额外 body 拷贝
     */
    auto post(const std::string& uri,
              std::string&& body,
              const std::string& content_type = "application/x-www-form-urlencoded",
              const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::POST, uri, std::move(body), content_type, headers);
    }

    /**
     * @brief 发送 PUT 请求
     * @param uri 请求 URI
     * @param body 请求体
     * @param content_type Content-Type
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto put(const std::string& uri,
             const std::string& body,
             const std::string& content_type = "application/json",
             const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::PUT, uri, body, content_type, headers);
    }

    /**
     * @brief 发送 DELETE 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto del(const std::string& uri,
             const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::DELETE, uri, "", "", headers);
    }

    /**
     * @brief 发送 HEAD 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto head(const std::string& uri,
              const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::HEAD, uri, "", "", headers);
    }

    /**
     * @brief 发送 OPTIONS 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto options(const std::string& uri,
                 const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::OPTIONS, uri, "", "", headers);
    }

    /**
     * @brief 发送 PATCH 请求
     * @param uri 请求 URI
     * @param body 请求体
     * @param content_type Content-Type
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto patch(const std::string& uri,
               const std::string& body,
               const std::string& content_type = "application/json",
               const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::PATCH, uri, body, content_type, headers);
    }

    /**
     * @brief 发送 TRACE 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto trace(const std::string& uri,
               const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::TRACE, uri, "", "", headers);
    }

    /**
     * @brief 发送 CONNECT 请求（隧道）
     * @param target_host 目标主机
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto tunnel(const std::string& target_host,
                const std::map<std::string, std::string>& headers = {}) {
        return create_request(HttpMethod::CONNECT, target_host, "", "", headers);
    }

    /**
     * @brief 发送自定义 HttpRequest
     * @param request HTTP 请求对象
     * @return 写入 awaitable
     */
    auto send_request(HttpRequest& request) {
        return m_writer.send_request(request);
    }

    /**
     * @brief 发送调用方已预先序列化好的完整 HTTP/1.x 请求字节
     * @param request 包含 start-line、headers、空行与 body 的完整请求报文
     * @return 请求-响应一体化 awaitable
     * @note 该接口复用 HttpSession 的超时、收包与响应解析状态机，但跳过 HttpRequest/Header 序列化
     * @note request 的所有权会转移到 awaitable 内部；await 完成前无需额外保持外部缓冲存活
     * @note 调用方必须自行保证报文格式合法，尤其是 Content-Length、Connection 与请求行
     */
    auto send_serialized_request(std::string request) {
        return detail::build_session_operation(*this, std::move(request));
    }

    /**
     * @brief 异步接收 HTTP 响应
     * @param response 待填充的响应对象
     * @return 读取 awaitable
     */
    auto get_response(HttpResponse& response) {
        return m_reader.get_response(response);
    }

    /**
     * @brief 只读取响应头，将同批到达的 body 字节留给后续增量读取。
     * @param header 头部对象
     * @return 响应头读取等待体，通过 co_await 取得读取结果
     */
    auto get_response_header(HttpResponseHeader& header) {
        return m_reader.get_response_header(header);
    }

    /**
     * @brief 发送 chunked 编码数据块
     * @param data 数据内容
     * @param is_last 是否为最后一个 chunk
     * @return 写入 awaitable
     */
    auto send_chunk(const std::string& data, bool is_last = false) {
        return m_writer.send_chunk(data, is_last);
    }

    /**
     * @brief 增量读取下一个已完整 HTTP chunk。
     * @param chunk_data 分块数据
     * @param parser 解析器
     * @return 下一块 HTTP chunk 的读取等待体，通过 co_await 取得读取结果
     */
    auto get_next_chunk(std::string& chunk_data, ChunkParser& parser) {
        return m_reader.get_next_chunk(chunk_data, parser);
    }

private:
    /**
     * @brief 内部创建并发送 HTTP 请求
     * @param method HTTP 方法
     * @param uri 请求 URI
     * @param body 请求体
     * @param content_type Content-Type
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto create_request(HttpMethod method,
                       const std::string& uri,
                       std::string body,
                       const std::string& content_type,
                       const std::map<std::string, std::string>& headers) {
        HttpRequest request;
        HttpRequestHeader header;

        header.method() = method;
        header.uri() = uri;
        header.version() = HttpVersion::HttpVersion_1_1;

        if (!body.empty() && !content_type.empty()) {
            header.header_pairs().add_header_pair("Content-Type", content_type);
            header.header_pairs().add_header_pair("Content-Length", std::to_string(body.size()));
        }

        for (const auto& [key, value] : headers) {
            header.header_pairs().add_header_pair(key, value);
        }

        request.set_header(std::move(header));
        if (!body.empty()) {
            request.set_body_str(std::move(body));
        }

        return detail::build_session_operation(*this, std::move(request));
    }

    SocketType& m_socket;                                    ///< Socket 引用
    RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> m_ring_buffer;                              ///< 环形缓冲区
    HttpReaderSetting m_reader_setting;                      ///< 读取器配置
    HttpWriterSetting m_writer_setting;                      ///< 写入器配置
    HttpReaderImpl<SocketType> m_reader;                     ///< 读取器
    HttpWriterImpl<SocketType> m_writer;                     ///< 写入器
};

using HttpSession = HttpSessionImpl<AsyncTcpSocket>; ///< HTTP 明文会话类型别名

} // namespace galay::http

#ifdef GALAY_SSL_FEATURE_ENABLED
namespace galay::http {
using HttpsSession = HttpSessionImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_SESSION_H
