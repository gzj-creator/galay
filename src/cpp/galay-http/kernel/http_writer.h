/**
 * @file http_writer.h
 * @brief HTTP 写入器，基于异步状态机将 HTTP 消息写入 Socket
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HttpWriterImpl 模板类，支持将 HTTP 响应、请求和 chunked 数据
 * 写入 TcpSocket 或 SslSocket。内部使用 iovec 零拷贝技术，
 * 结合异步状态机实现高效的非阻塞写入。
 */

#ifndef GALAY_HTTP_WRITER_H
#define GALAY_HTTP_WRITER_H

#include "writer_settings.h"
#include "../common/http_log.h"
#include "../common/iovec_utils.h"
#include "../protoc/http_response.h"
#include "../protoc/http_request.h"
#include "../protoc/http_error.h"
#include "../protoc/http_chunk.h"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/async/tcp_socket.h"
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <sys/uio.h>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-utils/cache/bytes.hpp"
#include "../../galay-ssl/async/ssl_await.h"
#include "../../galay-ssl/async/ssl_socket.h"
#endif

namespace galay::http
{

using namespace galay::kernel;
using namespace galay::async;
#ifdef GALAY_SSL_FEATURE_ENABLED
using ::galay::utils::Bytes;
#endif

template<typename SocketType>
class HttpWriterImpl;

/**
 * @brief TCP Socket 类型判断特征
 */
template<typename T>
struct is_tcp_socket : std::false_type {};

template<>
struct is_tcp_socket<TcpSocket> : std::true_type {};

/**
 * @brief 判断 T 是否为 TCP Socket 的内联常量
 */
template<typename T>
inline constexpr bool is_tcp_socket_v = is_tcp_socket<T>::value;

/**
 * @brief SSL Socket 类型判断特征（用于 writer）
 */
template<typename T>
struct is_http_writer_ssl_socket : std::false_type {};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<>
struct is_http_writer_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

/**
 * @brief 判断 T 是否为 SSL Socket 的内联常量（用于 writer）
 */
template<typename T>
inline constexpr bool is_http_writer_ssl_socket_v = is_http_writer_ssl_socket<T>::value;

namespace detail {

/**
 * @brief HTTP TCP 写入状态机
 * @tparam SocketType Socket 类型
 * @tparam UseWritev 是否使用 writev（true）或 send（false）
 */
template<typename SocketType, bool UseWritev>
struct HttpTcpWriteMachine {
    using result_type = std::expected<bool, HttpError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    explicit HttpTcpWriteMachine(HttpWriterImpl<SocketType>* writer)
        : m_writer(writer) {}

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
            return MachineAction<result_type>::complete(true);
        }

        if constexpr (UseWritev) {
            const auto* iov_data = m_writer->getIovecsData();
            const size_t iov_count = m_writer->getIovecsCount();
            if (iov_data == nullptr || iov_count == 0) {
                failWithMessage("No remaining iovec to write");
                return MachineAction<result_type>::complete(std::move(*m_result));
            }
            return MachineAction<result_type>::waitWritev(iov_data, iov_count);
        } else {
            return MachineAction<result_type>::waitWrite(
                m_writer->bufferData() + m_writer->sentBytes(),
                m_writer->getRemainingBytes());
        }
    }

    void onRead(std::expected<size_t, IOError>) {}

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            failWithIo(result.error(), UseWritev ? "writev" : "send");
            return;
        }

        const size_t written = result.value();
        if (written > 0) {
            if constexpr (UseWritev) {
                m_writer->updateRemainingWritev(written);
            } else {
                m_writer->updateRemaining(written);
            }
        }

        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
            return;
        }

        if constexpr (UseWritev) {
            if (m_writer->getIovecsData() == nullptr || m_writer->getIovecsCount() == 0) {
                failWithMessage("No remaining iovec to write");
            }
        }
    }

private:
    void failWithIo(const IOError& io_error, const char* op) {
        m_result = std::unexpected(HttpError(kSendError, io_error.message()));
    }

    void failWithMessage(const char* message) {
        m_result = std::unexpected(HttpError(kSendError, message));
    }

    HttpWriterImpl<SocketType>* m_writer;
    std::optional<result_type> m_result;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
/**
 * @brief HTTP SSL 发送状态机
 * @tparam SocketType Socket 类型
 */
template<typename SocketType>
struct HttpSslSendMachine {
    using result_type = std::expected<bool, HttpError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    explicit HttpSslSendMachine(HttpWriterImpl<SocketType>* writer)
        : m_writer(writer) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
            return galay::ssl::SslMachineAction<result_type>::complete(true);
        }

        return galay::ssl::SslMachineAction<result_type>::send(
            m_writer->bufferData() + m_writer->sentBytes(),
            m_writer->getRemainingBytes());
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}
    void onRecv(std::expected<Bytes, galay::ssl::SslError>) {}
    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_writer->updateRemaining(m_writer->getRemainingBytes());
            m_result = std::unexpected(HttpError(kSendError, result.error().message()));
            return;
        }

        if (result.value() == 0) {
            m_writer->updateRemaining(m_writer->getRemainingBytes());
            m_result = std::unexpected(HttpError(kSendError, "SSL send returned zero bytes"));
            return;
        }

        m_writer->updateRemaining(result.value());
        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
        }
    }

private:
    HttpWriterImpl<SocketType>* m_writer;
    std::optional<result_type> m_result;
};
#endif

/**
 * @brief 构建异步发送操作
 * @tparam SocketType Socket 类型
 * @tparam UseWritev 是否使用 writev
 * @param socket Socket 引用
 * @param writer 写入器引用
 * @return 可 co_await 的异步操作对象
 */
template<typename SocketType, bool UseWritev>
auto buildSendAwaitable(SocketType& socket, HttpWriterImpl<SocketType>& writer) {
    using ResultType = std::expected<bool, HttpError>;
    if constexpr (is_http_writer_ssl_socket_v<SocketType>) {
#ifdef GALAY_SSL_FEATURE_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   &socket,
                   HttpSslSendMachine<SocketType>(&writer))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   HttpTcpWriteMachine<SocketType, UseWritev>(&writer))
            .build();
    }
}

} // namespace detail

/**
 * @brief HTTP 写入器模板类
 * @tparam SocketType Socket 类型（TcpSocket 或 SslSocket）
 * @details 将 HTTP 响应、请求和 chunked 数据异步写入 Socket。
 *          TCP 模式使用 writev 零拷贝，SSL 模式使用 send。
 */
template<typename SocketType>
class HttpWriterImpl
{
public:
    struct FastPathCounters {
        size_t ssl_coalesced_layout_hits = 0; ///< SSL 合并布局命中次数
    };

    /**
     * @brief 构造函数
     * @param setting 写入器配置
     * @param socket Socket 引用
     */
    HttpWriterImpl(const HttpWriterSetting& setting, SocketType& socket)
        : m_setting(setting)
        , m_socket(&socket)
        , m_remaining_bytes(0)
    {
    }

    /**
     * @brief 异步发送 HTTP 响应
     * @param response HTTP 响应对象
     * @return 可 co_await 的异步操作，成功返回 true，失败返回 HttpError
     */
    auto sendResponse(HttpResponse& response) {
        if (m_remaining_bytes == 0) {
            logResponseStatus(response.header().code());

            if constexpr (is_tcp_socket_v<SocketType>) {
                m_body_buffer = response.getBodyStr();

                if (!response.header().isChunked()) {
                    response.header().headerPairs().addHeaderPairIfNotExist(
                        "Content-Length",
                        std::to_string(m_body_buffer.size()));
                }

                m_buffer = response.header().toString();
                prepareTcpSendLayout();
            } else {
                if (!response.header().isChunked()) {
                    response.header().headerPairs().addHeaderPairIfNotExist(
                        "Content-Length",
                        std::to_string(response.bodyStr().size()));
                }
                prepareSslSendLayout(response.header().toString(), response.bodyStr());
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return makeWritevAwaitable();
        } else {
            return makeSendAwaitable();
        }
    }

    /**
     * @brief 异步发送 HTTP 请求
     * @param request HTTP 请求对象
     * @return 可 co_await 的异步操作
     */
    auto sendRequest(HttpRequest& request) {
        if (m_remaining_bytes == 0) {
            if constexpr (is_tcp_socket_v<SocketType>) {
                m_body_buffer = request.bodyStr();

                if (!request.header().isChunked()) {
                    request.header().headerPairs().addHeaderPairIfNotExist(
                        "Content-Length",
                        std::to_string(m_body_buffer.size()));
                }

                m_buffer = request.header().toString();
                prepareTcpSendLayout();
            } else {
                if (!request.header().isChunked()) {
                    request.header().headerPairs().addHeaderPairIfNotExist(
                        "Content-Length",
                        std::to_string(request.bodyStr().size()));
                }
                prepareSslSendLayout(request.header().toString(), request.bodyStr());
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return makeWritevAwaitable();
        } else {
            return makeSendAwaitable();
        }
    }

    /**
     * @brief 异步发送 HTTP 响应头
     * @param header HTTP 响应头
     * @return 可 co_await 的异步操作
     */
    auto sendHeader(HttpResponseHeader&& header) {
        if (m_remaining_bytes == 0) {
            logResponseStatus(header.code());
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        return makeSendAwaitable();
    }

    /**
     * @brief 异步发送 HTTP 请求头
     * @param header HTTP 请求头
     * @return 可 co_await 的异步操作
     */
    auto sendHeader(HttpRequestHeader&& header) {
        if (m_remaining_bytes == 0) {
            m_buffer = header.toString();
            m_remaining_bytes = m_buffer.size();
        }

        return makeSendAwaitable();
    }

    /**
     * @brief 异步发送原始数据（移动语义）
     * @param data 待发送数据
     * @return 可 co_await 的异步操作
     */
    auto send(std::string&& data) {
        if (m_remaining_bytes == 0) {
            clearExternalBuffer();
            m_buffer = std::move(data);
            m_remaining_bytes = m_buffer.size();
        }

        return makeSendAwaitable();
    }

    /**
     * @brief 异步发送原始数据（指针+长度）
     * @param buffer 数据指针
     * @param length 数据长度
     * @return 可 co_await 的异步操作
     */
    auto send(const char* buffer, size_t length) {
        if (m_remaining_bytes == 0) {
            clearExternalBuffer();
            m_buffer.assign(buffer, length);
            m_remaining_bytes = m_buffer.size();
        }

        return makeSendAwaitable();
    }

    /**
     * @brief 发送外部持有的连续字节视图
     * @param data 待发送的连续只读字节视图
     * @return 发送等待体
     * @note 调用方必须保证 data 对应的底层存储在 await 完成前保持有效
     * @note 该接口适用于静态响应或连接级缓存响应，避免重复复制到 writer 内部缓冲区
     */
    auto sendView(std::string_view data) {
        if (m_remaining_bytes == 0) {
            m_buffer.clear();
            m_body_buffer.clear();
            m_writev_cursor.reset(std::vector<iovec>{});
            m_external_buffer = data.data();
            m_external_buffer_size = data.size();
            m_remaining_bytes = data.size();
        }

        return makeSendAwaitable();
    }

    /**
     * @brief 异步发送 chunked 编码数据块
     * @param data 数据内容
     * @param is_last 是否为最后一个 chunk
     * @return 可 co_await 的异步操作
     */
    auto sendChunk(const std::string& data, bool is_last = false) {
        if (m_remaining_bytes == 0) {
            clearExternalBuffer();
            m_buffer = Chunk::toChunk(data, is_last);
            m_remaining_bytes = m_buffer.size();
        }

        return makeSendAwaitable();
    }

    void updateRemaining(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
            m_body_buffer.clear();
            clearExternalBuffer();
            m_writev_cursor.reset(std::vector<iovec>{});
        } else {
            m_remaining_bytes -= bytes_sent;
        }
    }

    void updateRemainingWritev(size_t bytes_sent) {
        const size_t advanced = m_writev_cursor.advance(bytes_sent);
        if (advanced >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
            m_body_buffer.clear();
            clearExternalBuffer();
            m_writev_cursor.reset(std::vector<iovec>{});
        } else {
            m_remaining_bytes -= advanced;
        }
    }

    size_t getRemainingBytes() const {
        return m_remaining_bytes;
    }

    const char* bufferData() const {
        return m_external_buffer != nullptr ? m_external_buffer : m_buffer.data();
    }

    size_t sentBytes() const {
        return currentBufferSize() - m_remaining_bytes;
    }

    std::vector<iovec> getIovecsCopy() const {
        std::vector<iovec> out;
        m_writev_cursor.exportWindow(out);
        return out;
    }

    void copyIovecsTo(std::vector<iovec>& out) const {
        m_writev_cursor.exportWindow(out);
    }

    const iovec* getIovecsData() const {
        return m_writev_cursor.data();
    }

    size_t getIovecsCount() const {
        return m_writev_cursor.count();
    }

private:
    auto makeSendAwaitable() {
        return detail::buildSendAwaitable<SocketType, false>(*m_socket, *this);
    }

    auto makeWritevAwaitable() {
        return detail::buildSendAwaitable<SocketType, true>(*m_socket, *this);
    }

    void prepareTcpSendLayout() {
        const size_t total_size = m_buffer.size() + m_body_buffer.size();
        const size_t coalesce_threshold = m_setting.getWritevCoalesceThreshold();

        if (coalesce_threshold > 0 && total_size <= coalesce_threshold) {
            if (!m_body_buffer.empty()) {
                m_buffer.append(m_body_buffer);
                m_body_buffer.clear();
            }
            std::vector<iovec> iovecs;
            iovecs.reserve(1);
            iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
            m_writev_cursor.reset(std::move(iovecs));
            m_remaining_bytes = m_writev_cursor.remainingBytes();
            return;
        }

        std::vector<iovec> iovecs;
        iovecs.reserve(2);
        iovecs.push_back({const_cast<char*>(m_buffer.data()), m_buffer.size()});
        if (!m_body_buffer.empty()) {
            iovecs.push_back({const_cast<char*>(m_body_buffer.data()), m_body_buffer.size()});
        }
        m_writev_cursor.reset(std::move(iovecs));
        m_remaining_bytes = m_writev_cursor.remainingBytes();
    }

    void prepareSslSendLayout(std::string header, std::string_view body) {
        clearExternalBuffer();
        m_buffer.clear();
        m_buffer.reserve(header.size() + body.size());
        m_buffer.append(std::move(header));
        if (!body.empty()) {
            m_buffer.append(body.data(), body.size());
        }
        m_remaining_bytes = m_buffer.size();
        ++m_fast_path_counters.ssl_coalesced_layout_hits;
    }

    static void logResponseStatus(HttpStatusCode code) {
        const int status = static_cast<int>(code);
        if (status >= 500) {
            HTTP_LOG_ERROR("[response]", "status={}", status);
        } else if (status >= 400) {
            HTTP_LOG_WARN("[response]", "status={}", status);
        } else {
            HTTP_LOG_DEBUG("[response]", "status={}", status);
        }
    }

    size_t currentBufferSize() const {
        return m_external_buffer != nullptr ? m_external_buffer_size : m_buffer.size();
    }

    void clearExternalBuffer() {
        m_external_buffer = nullptr;
        m_external_buffer_size = 0;
    }

    HttpWriterSetting m_setting;
    SocketType* m_socket;
    std::string m_buffer;
    size_t m_remaining_bytes;
    std::string m_body_buffer;
    const char* m_external_buffer = nullptr;
    size_t m_external_buffer_size = 0;
    IoVecCursor m_writev_cursor;
    FastPathCounters m_fast_path_counters;
};

using HttpWriter = HttpWriterImpl<TcpSocket>;

} // namespace galay::http

#ifdef GALAY_SSL_FEATURE_ENABLED
namespace galay::http {
using HttpsWriter = HttpWriterImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_WRITER_H
