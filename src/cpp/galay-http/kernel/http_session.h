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
#include "../../galay-kernel/async/tcp_socket.h"
#include "../../galay-utils/cache/bytes.hpp"
#include "../../galay-utils/cache/ring_buffer.hpp"
#include "../../galay-kernel/core/awaitable.h"
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>

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
        , m_send_buffer(m_request.toString()) {}

    /**
     * @brief 从已序列化的请求字符串构造
     * @param session 所属会话
     * @param serialized_request 完整的 HTTP 请求报文
     */
    HttpSessionState(HttpSessionImpl<SocketType>& session, std::string&& serialized_request)
        : m_session(&session)
        , m_send_buffer(std::move(serialized_request)) {}

    bool sendCompleted() const { ///< 判断请求是否已完全发送
        return m_send_offset >= m_send_buffer.size();
    }

    const char* sendBuffer() const { ///< 获取当前发送缓冲区指针
        return m_send_buffer.data() + m_send_offset;
    }

    size_t sendRemaining() const { ///< 获取剩余待发送字节数
        return m_send_buffer.size() - m_send_offset;
    }

    void onBytesSent(size_t sent) { ///< 处理已发送字节数
        m_send_offset += sent;
    }

    /**
     * @brief 从 RingBuffer 中尝试解析 HTTP 响应
     * @return 解析完成返回 true
     */
    bool parseFromRingBuffer() {
        auto read_iovecs = borrowReadIovecs(m_session->getRingBuffer());
        if (read_iovecs.empty()) {
            return false;
        }

        if (IoVecWindow::buildWindow(read_iovecs, m_parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] =
            m_response.fromIOVec(m_parse_iovecs, m_session->getReaderSetting().getMaxBodySize());
        if (consumed > 0) {
            m_session->getRingBuffer().consume(static_cast<size_t>(consumed));
        }

        if (error_code == kHeaderInComplete || error_code == kIncomplete) {
            if (m_total_received >= m_session->getReaderSetting().getMaxHeaderSize() &&
                !m_response.isComplete()) {
                setParseError(HttpError(kHeaderTooLarge));
                return true;
            }
            return false;
        }

        if (error_code != kNoError) {
            setParseError(HttpError(error_code));
            return true;
        }

        if (!m_response.isComplete()) {
            return false;
        }

        m_response_value = std::optional<HttpResponse>(std::move(m_response));
        return true;
    }

    bool prepareRecvWindow() { ///< 准备 TCP 接收窗口
        m_write_iovecs = borrowWriteIovecs(m_session->getRingBuffer());
        if (m_write_iovecs.empty()) {
            setParseError(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) { ///< 准备 SSL 接收窗口
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            setParseError(HttpError(kHeaderTooLarge));
            return false;
        }
        return true;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); }
    size_t recvIovecsCount() const { return m_write_iovecs.size(); }

    void setSendError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kTimeout)) {
            m_error = HttpError(kRequestTimeOut, io_error.message());
            return;
        }
        m_error = HttpError(kSendError, io_error.message());
    }

    void setRecvError(const IOError& io_error) {
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
    void setSslSendError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_error = HttpError(kConnectionClose, error.message());
            return;
        }
        m_error = HttpError(kSendError, error.message());
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_error = HttpError(kConnectionClose, error.message());
            return;
        }
        m_error = HttpError(kRecvError, error.message());
    }
#endif

    void onPeerClosed() {
        m_error = HttpError(kConnectionClose);
    }

    void onBytesReceived(size_t recv_bytes) {
        m_session->getRingBuffer().produce(recv_bytes);
        m_total_received += recv_bytes;
    }

    void setParseError(HttpError&& error) {
        m_error = std::move(error);
    }

    ResultType takeResult() {
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

        if (!m_state->sendCompleted()) {
            return MachineAction<result_type>::waitWrite(
                m_state->sendBuffer(),
                m_state->sendRemaining());
        }

        if (m_state->parseFromRingBuffer()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepareRecvWindow()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitReadv(
            m_state->recvIovecsData(),
            m_state->recvIovecsCount());
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        if (result.value() == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setSendError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesSent(result.value());
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

        if (!m_state->sendCompleted()) {
            return galay::ssl::SslMachineAction<result_type>::send(
                m_state->sendBuffer(),
                m_state->sendRemaining());
        }

        if (m_state->parseFromRingBuffer()) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_state->prepareRecvWindow(recv_buffer, recv_length)) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslSendError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesSent(result.value());
    }

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

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
auto buildSessionOperation(HttpSessionImpl<SocketType>& session, HttpRequest&& request) {
    using State = HttpSessionState<SocketType>;
    using ResultType = typename State::ResultType;
    auto state = std::make_shared<State>(session, std::move(request));

    if constexpr (std::is_same_v<SocketType, TcpSocket>) {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   session.getSocket().controller(),
                   HttpSessionTcpMachine<SocketType>(std::move(state)))
            .build();
    } else {
#ifdef GALAY_SSL_FEATURE_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   session.getSocket().controller(),
                   &session.getSocket(),
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
auto buildSessionOperation(HttpSessionImpl<SocketType>& session, std::string&& serialized_request) {
    using State = HttpSessionState<SocketType>;
    using ResultType = typename State::ResultType;
    auto state = std::make_shared<State>(session, std::move(serialized_request));

    if constexpr (std::is_same_v<SocketType, TcpSocket>) {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   session.getSocket().controller(),
                   HttpSessionTcpMachine<SocketType>(std::move(state)))
            .build();
    } else {
#ifdef GALAY_SSL_FEATURE_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   session.getSocket().controller(),
                   &session.getSocket(),
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
 * @tparam SocketType Socket 类型（TcpSocket 或 SslSocket）
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

    HttpReaderImpl<SocketType>& getReader() { return m_reader; } ///< 获取读取器引用
    HttpWriterImpl<SocketType>& getWriter() { return m_writer; } ///< 获取写入器引用
    SocketType& getSocket() { return m_socket; } ///< 获取底层 Socket 引用
    RingBuffer<>& getRingBuffer() { return m_ring_buffer; } ///< 获取 RingBuffer 引用
    const HttpReaderSetting& getReaderSetting() const { return m_reader_setting; } ///< 获取读取器配置

    /**
     * @brief 发送 GET 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto get(const std::string& uri,
             const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::GET, uri, "", "", headers);
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
        return createRequest(HttpMethod::POST, uri, body, content_type, headers);
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
        return createRequest(HttpMethod::POST, uri, std::move(body), content_type, headers);
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
        return createRequest(HttpMethod::PUT, uri, body, content_type, headers);
    }

    /**
     * @brief 发送 DELETE 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto del(const std::string& uri,
             const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::DELETE, uri, "", "", headers);
    }

    /**
     * @brief 发送 HEAD 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto head(const std::string& uri,
              const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::HEAD, uri, "", "", headers);
    }

    /**
     * @brief 发送 OPTIONS 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto options(const std::string& uri,
                 const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::OPTIONS, uri, "", "", headers);
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
        return createRequest(HttpMethod::PATCH, uri, body, content_type, headers);
    }

    /**
     * @brief 发送 TRACE 请求
     * @param uri 请求 URI
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto trace(const std::string& uri,
               const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::TRACE, uri, "", "", headers);
    }

    /**
     * @brief 发送 CONNECT 请求（隧道）
     * @param target_host 目标主机
     * @param headers 额外请求头
     * @return 请求-响应一体化 awaitable
     */
    auto tunnel(const std::string& target_host,
                const std::map<std::string, std::string>& headers = {}) {
        return createRequest(HttpMethod::CONNECT, target_host, "", "", headers);
    }

    /**
     * @brief 发送自定义 HttpRequest
     * @param request HTTP 请求对象
     * @return 写入 awaitable
     */
    auto sendRequest(HttpRequest& request) {
        return m_writer.sendRequest(request);
    }

    /**
     * @brief 发送调用方已预先序列化好的完整 HTTP/1.x 请求字节
     * @param request 包含 start-line、headers、空行与 body 的完整请求报文
     * @return 请求-响应一体化 awaitable
     * @note 该接口复用 HttpSession 的超时、收包与响应解析状态机，但跳过 HttpRequest/Header 序列化
     * @note request 的所有权会转移到 awaitable 内部；await 完成前无需额外保持外部缓冲存活
     * @note 调用方必须自行保证报文格式合法，尤其是 Content-Length、Connection 与请求行
     */
    auto sendSerializedRequest(std::string request) {
        return detail::buildSessionOperation(*this, std::move(request));
    }

    /**
     * @brief 异步接收 HTTP 响应
     * @param response 待填充的响应对象
     * @return 读取 awaitable
     */
    auto getResponse(HttpResponse& response) {
        return m_reader.getResponse(response);
    }

    /**
     * @brief 发送 chunked 编码数据块
     * @param data 数据内容
     * @param is_last 是否为最后一个 chunk
     * @return 写入 awaitable
     */
    auto sendChunk(const std::string& data, bool is_last = false) {
        return m_writer.sendChunk(data, is_last);
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
    auto createRequest(HttpMethod method,
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
            header.headerPairs().addHeaderPair("Content-Type", content_type);
            header.headerPairs().addHeaderPair("Content-Length", std::to_string(body.size()));
        }

        for (const auto& [key, value] : headers) {
            header.headerPairs().addHeaderPair(key, value);
        }

        request.setHeader(std::move(header));
        if (!body.empty()) {
            request.setBodyStr(std::move(body));
        }

        return detail::buildSessionOperation(*this, std::move(request));
    }

    SocketType& m_socket;                                    ///< Socket 引用
    RingBuffer<> m_ring_buffer;                              ///< 环形缓冲区
    HttpReaderSetting m_reader_setting;                      ///< 读取器配置
    HttpWriterSetting m_writer_setting;                      ///< 写入器配置
    HttpReaderImpl<SocketType> m_reader;                     ///< 读取器
    HttpWriterImpl<SocketType> m_writer;                     ///< 写入器
};

using HttpSession = HttpSessionImpl<TcpSocket>; ///< HTTP 明文会话类型别名

} // namespace galay::http

#ifdef GALAY_SSL_FEATURE_ENABLED
namespace galay::http {
using HttpsSession = HttpSessionImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_SESSION_H
