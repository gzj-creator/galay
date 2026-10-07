/**
 * @file http_writer.h
 * @brief HTTP 写入器，基于异步状态机将 HTTP 消息写入 Socket
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HttpWriterImpl 模板类，支持将 HTTP 响应、请求和 chunked 数据
 * 写入 AsyncTcpSocket 或 SslSocket。内部使用 iovec 零拷贝技术，
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
#include "../../galay-kernel/async/async_tcp.h"
#include <array>
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <sys/uio.h>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-utils/buffer/bytes.hpp"
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
struct is_tcp_socket<AsyncTcpSocket> : std::true_type {};

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

inline HttpError make_send_http_error(const IOError& io_error) {
    if (IOError::contains(io_error.code(), kTimeout)) {
        return HttpError(kSendTimeOut, io_error.message());
    }
    return HttpError(kSendError, io_error.message());
}

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

        if (m_writer->get_remaining_bytes() == 0) {
            m_result = true;
            return MachineAction<result_type>::complete(true);
        }

        if constexpr (UseWritev) {
            const auto* iov_data = m_writer->get_iovecs_data();
            const size_t iov_count = m_writer->get_iovecs_count();
            if (iov_data == nullptr || iov_count == 0) {
                fail_with_message("No remaining iovec to write");
                return MachineAction<result_type>::complete(std::move(*m_result));
            }
            return MachineAction<result_type>::wait_writev(iov_data, iov_count);
        } else {
            return MachineAction<result_type>::wait_write(
                m_writer->buffer_data() + m_writer->sent_bytes(),
                m_writer->get_remaining_bytes());
        }
    }

    void on_read(std::expected<size_t, IOError>) {}

    void on_write(std::expected<size_t, IOError> result) {
        if (!result) {
            fail_with_io(result.error(), UseWritev ? "writev" : "send");
            return;
        }

        const size_t written = result.value();
        if (written > 0) {
            if constexpr (UseWritev) {
                m_writer->update_remaining_writev(written);
            } else {
                m_writer->update_remaining(written);
            }
        }

        if (m_writer->get_remaining_bytes() == 0) {
            m_result = true;
            return;
        }

        if constexpr (UseWritev) {
            if (m_writer->get_iovecs_data() == nullptr || m_writer->get_iovecs_count() == 0) {
                fail_with_message("No remaining iovec to write");
            }
        }
    }

private:
    void fail_with_io(const IOError& io_error, const char*) {
        m_result = std::unexpected(make_send_http_error(io_error));
    }

    void fail_with_message(const char* message) {
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

        if (m_writer->get_remaining_bytes() == 0) {
            m_result = true;
            return galay::ssl::SslMachineAction<result_type>::complete(true);
        }

        return galay::ssl::SslMachineAction<result_type>::send(
            m_writer->buffer_data() + m_writer->sent_bytes(),
            m_writer->get_remaining_bytes());
    }

    void on_handshake(std::expected<void, galay::ssl::SslError>) {}
    void on_recv(std::expected<Bytes, galay::ssl::SslError>) {}
    void on_shutdown(std::expected<void, galay::ssl::SslError>) {}

    void on_send(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_writer->update_remaining(m_writer->get_remaining_bytes());
            m_result = std::unexpected(HttpError(result.error()));
            return;
        }

        if (result.value() == 0) {
            m_writer->update_remaining(m_writer->get_remaining_bytes());
            m_result = std::unexpected(HttpError(kSendError, "SSL send returned zero bytes"));
            return;
        }

        m_writer->update_remaining(result.value());
        if (m_writer->get_remaining_bytes() == 0) {
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
 * @return 可 co_await 的异步操作；co_await 结果为
 *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
 */
template<typename SocketType, bool UseWritev>
auto build_send_awaitable(SocketType& socket, HttpWriterImpl<SocketType>& writer) {
    using ResultType = std::expected<bool, HttpError>;
    if constexpr (is_http_writer_ssl_socket_v<SocketType>) {
#ifdef GALAY_SSL_FEATURE_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::from_state_machine(
                   socket.controller(),
                   &socket,
                   HttpSslSendMachine<SocketType>(&writer))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::from_state_machine(
                   socket.controller(),
                   HttpTcpWriteMachine<SocketType, UseWritev>(&writer))
            .build();
    }
}

} // namespace detail

/**
 * @brief HTTP 写入器模板类
 * @tparam SocketType Socket 类型（AsyncTcpSocket 或 SslSocket）
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
        m_writev_cursor.reserve(2);
    }

    /**
     * @brief 异步发送 HTTP 响应
     * @param response HTTP 响应对象
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     * @note 启动新发送时会复制响应体到 writer，不转移 response 的响应体
     */
    auto send_response(HttpResponse& response) {
        if (m_remaining_bytes == 0) {
            log_response_status(response.header().code());

            if constexpr (is_tcp_socket_v<SocketType>) {
                m_body_buffer = response.body_str();

                if (!response.header().is_chunked()) {
                    ensure_content_length(response.header().header_pairs(), m_body_buffer.size());
                }

                m_buffer = response.header().to_string();
                prepare_tcp_send_layout();
            } else {
                if (!response.header().is_chunked()) {
                    ensure_content_length(response.header().header_pairs(), response.body_str().size());
                }
                prepare_ssl_send_layout(response.header().to_string(), response.body_str());
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return with_configured_timeout(make_writev_awaitable());
        } else {
            return with_configured_timeout(make_send_awaitable());
        }
    }

    /**
     * @brief 异步发送 HTTP 响应（右值重载）
     * @param response HTTP 响应对象；启动新的 TCP 发送时会转移其响应体
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     * @note 启动新发送时，待发送数据会在返回异步操作前保存到 writer，不持有 response 引用
     */
    auto send_response(HttpResponse&& response) {
        if constexpr (is_tcp_socket_v<SocketType>) {
            if (m_remaining_bytes == 0) {
                log_response_status(response.header().code());
                m_body_buffer = response.get_body_str();

                if (!response.header().is_chunked()) {
                    ensure_content_length(response.header().header_pairs(), m_body_buffer.size());
                }

                m_buffer = response.header().to_string();
                prepare_tcp_send_layout();
            }

            return with_configured_timeout(make_writev_awaitable());
        } else {
            return send_response(response);
        }
    }

    /**
     * @brief 异步发送 HTTP 请求
     * @param request HTTP 请求对象
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     */
    auto send_request(HttpRequest& request) {
        if (m_remaining_bytes == 0) {
            if constexpr (is_tcp_socket_v<SocketType>) {
                m_body_buffer = request.body_str();

                if (!request.header().is_chunked()) {
                    ensure_content_length(request.header().header_pairs(), m_body_buffer.size());
                }

                m_buffer = request.header().to_string();
                prepare_tcp_send_layout();
            } else {
                if (!request.header().is_chunked()) {
                    ensure_content_length(request.header().header_pairs(), request.body_str().size());
                }
                prepare_ssl_send_layout(request.header().to_string(), request.body_str());
            }
        }

        if constexpr (is_tcp_socket_v<SocketType>) {
            return with_configured_timeout(make_writev_awaitable());
        } else {
            return with_configured_timeout(make_send_awaitable());
        }
    }

    /**
     * @brief 异步发送 HTTP 请求（右值重载）
     * @param request HTTP 请求对象；启动新的 TCP 发送时会转移其请求体
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     * @note 启动新发送时，待发送数据会在返回异步操作前保存到 writer，不持有 request 引用
     */
    auto send_request(HttpRequest&& request) {
        if constexpr (is_tcp_socket_v<SocketType>) {
            if (m_remaining_bytes == 0) {
                m_body_buffer = request.get_body_str();

                if (!request.header().is_chunked()) {
                    ensure_content_length(request.header().header_pairs(), m_body_buffer.size());
                }

                m_buffer = request.header().to_string();
                prepare_tcp_send_layout();
            }

            return with_configured_timeout(make_writev_awaitable());
        } else {
            return send_request(request);
        }
    }

    /**
     * @brief 异步发送 HTTP 响应头
     * @param header HTTP 响应头
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     * @note 启动新发送时，待发送数据会在返回异步操作前保存到 writer，不持有 header 引用
     */
    auto send_header(HttpResponseHeader& header) {
        if (m_remaining_bytes == 0) {
            log_response_status(header.code());
            m_buffer = header.to_string();
            m_remaining_bytes = m_buffer.size();
        }

        return with_configured_timeout(make_send_awaitable());
    }

    /**
     * @brief 异步发送 HTTP 响应头（右值重载）
     * @param header HTTP 响应头
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     */
    auto send_header(HttpResponseHeader&& header) {
        return send_header(header);
    }

    /**
     * @brief 异步发送 HTTP 请求头
     * @param header HTTP 请求头
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     * @note 启动新发送时，待发送数据会在返回异步操作前保存到 writer，不持有 header 引用
     */
    auto send_header(HttpRequestHeader& header) {
        if (m_remaining_bytes == 0) {
            m_buffer = header.to_string();
            m_remaining_bytes = m_buffer.size();
        }

        return with_configured_timeout(make_send_awaitable());
    }

    /**
     * @brief 异步发送 HTTP 请求头（右值重载）
     * @param header HTTP 请求头
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     */
    auto send_header(HttpRequestHeader&& header) {
        return send_header(header);
    }

    /**
     * @brief 异步发送原始数据（移动语义）
     * @param data 待发送数据
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     */
    auto send(std::string&& data) {
        if (m_remaining_bytes == 0) {
            clear_external_buffer();
            m_buffer = std::move(data);
            m_remaining_bytes = m_buffer.size();
        }

        return with_configured_timeout(make_send_awaitable());
    }

    /**
     * @brief 异步发送原始数据（指针+长度）
     * @param buffer 数据指针
     * @param length 数据长度
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     */
    auto send(const char* buffer, size_t length) {
        if (m_remaining_bytes == 0) {
            clear_external_buffer();
            m_buffer.assign(buffer, length);
            m_remaining_bytes = m_buffer.size();
        }

        return with_configured_timeout(make_send_awaitable());
    }

    /**
     * @brief 发送外部持有的连续字节视图
     * @param data 待发送的连续只读字节视图
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     * @note 调用方必须保证 data 对应的底层存储在 await 完成前保持有效
     * @note 该接口适用于静态响应或连接级缓存响应，避免重复复制到 writer 内部缓冲区
     */
    auto send_view(std::string_view data) {
        if (m_remaining_bytes == 0) {
            m_buffer.clear();
            m_body_buffer.clear();
            m_writev_cursor.clear();
            m_external_buffer = data.data();
            m_external_buffer_size = data.size();
            m_remaining_bytes = data.size();
        }

        return with_configured_timeout(make_send_awaitable());
    }

    /**
     * @brief 异步发送 chunked 编码数据块
     * @param data 数据内容
     * @param is_last 是否为最后一个 chunk
     * @return 可 co_await 的异步操作；co_await 结果为
     *         std::expected<bool, HttpError>，成功值为 true，失败时 error() 为 HttpError
     */
    auto send_chunk(const std::string& data, bool is_last = false) {
        if (m_remaining_bytes == 0) {
            clear_external_buffer();
            m_buffer = Chunk::to_chunk(data, is_last);
            m_remaining_bytes = m_buffer.size();
        }

        return with_configured_timeout(make_send_awaitable());
    }

    void update_remaining(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
            m_body_buffer.clear();
            clear_external_buffer();
            m_writev_cursor.clear();
        } else {
            m_remaining_bytes -= bytes_sent;
        }
    }

    void update_remaining_writev(size_t bytes_sent) {
        const size_t advanced = m_writev_cursor.advance(bytes_sent);
        if (advanced >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
            m_body_buffer.clear();
            clear_external_buffer();
            m_writev_cursor.clear();
        } else {
            m_remaining_bytes -= advanced;
        }
    }

    size_t get_remaining_bytes() const {
        return m_remaining_bytes;
    }

    const char* buffer_data() const {
        return m_external_buffer != nullptr ? m_external_buffer : m_buffer.data();
    }

    size_t sent_bytes() const {
        return current_buffer_size() - m_remaining_bytes;
    }

    std::vector<iovec> get_iovecs_copy() const {
        std::vector<iovec> out;
        m_writev_cursor.export_window(out);
        return out;
    }

    void copy_iovecs_to(std::vector<iovec>& out) const {
        m_writev_cursor.export_window(out);
    }

    const iovec* get_iovecs_data() const {
        return m_writev_cursor.data();
    }

    size_t get_iovecs_count() const {
        return m_writev_cursor.count();
    }

private:
    template<typename Awaitable>
    auto with_configured_timeout(Awaitable&& awaitable) {
        return std::forward<Awaitable>(awaitable).timeout(
            std::chrono::milliseconds(m_setting.get_send_timeout()));
    }

    auto make_send_awaitable() {
        return detail::build_send_awaitable<SocketType, false>(*m_socket, *this);
    }

    auto make_writev_awaitable() {
        return detail::build_send_awaitable<SocketType, true>(*m_socket, *this);
    }

    static void ensure_content_length(HeaderPair& headers, size_t size) {
        const HttpErrorCode result = headers.add_header_pair_if_not_exist(
            "Content-Length",
            std::to_string(size));
        if (result != kNoError && result != kHeaderPairExist) {
            HTTP_LOG_WARN("[writer] [content-length-header-fail]", "code={}", static_cast<int>(result));
        }
    }

    void prepare_tcp_send_layout() {
        const size_t total_size = m_buffer.size() + m_body_buffer.size();
        const size_t coalesce_threshold = m_setting.get_writev_coalesce_threshold();

        if (coalesce_threshold > 0 && total_size <= coalesce_threshold) {
            if (!m_body_buffer.empty()) {
                std::string& appended = m_buffer.append(m_body_buffer);
                if (&appended != &m_buffer) {
                    HTTP_LOG_WARN("[writer] [tcp-coalesce-append-unexpected]", "body_size={}", m_body_buffer.size());
                }
                m_body_buffer.clear();
            }
            std::array<iovec, 2> iovecs{};
            size_t iov_count = 0;
            iovecs[iov_count++] = {const_cast<char*>(m_buffer.data()), m_buffer.size()};
            m_writev_cursor.reset(iovecs, iov_count);
            m_remaining_bytes = m_writev_cursor.remaining_bytes();
            return;
        }

        std::array<iovec, 2> iovecs{};
        size_t iov_count = 0;
        iovecs[iov_count++] = {const_cast<char*>(m_buffer.data()), m_buffer.size()};
        if (!m_body_buffer.empty()) {
            iovecs[iov_count++] = {const_cast<char*>(m_body_buffer.data()), m_body_buffer.size()};
        }
        m_writev_cursor.reset(iovecs, iov_count);
        m_remaining_bytes = m_writev_cursor.remaining_bytes();
    }

    void prepare_ssl_send_layout(std::string header, std::string_view body) {
        clear_external_buffer();
        m_buffer.clear();
        m_buffer.reserve(header.size() + body.size());
        std::string& header_appended = m_buffer.append(header);
        if (&header_appended != &m_buffer) {
            HTTP_LOG_WARN("[writer] [ssl-header-append-unexpected]", "header_size={}", header.size());
        }
        if (!body.empty()) {
            std::string& body_appended = m_buffer.append(body.data(), body.size());
            if (&body_appended != &m_buffer) {
                HTTP_LOG_WARN("[writer] [ssl-body-append-unexpected]", "body_size={}", body.size());
            }
        }
        m_remaining_bytes = m_buffer.size();
        ++m_fast_path_counters.ssl_coalesced_layout_hits;
    }

    static void log_response_status(HttpStatusCode code) {
        const int status = static_cast<int>(code);
        if (status >= 500) {
            HTTP_LOG_ERROR("[response]", "status={}", status);
        } else if (status >= 400) {
            HTTP_LOG_WARN("[response]", "status={}", status);
        } else {
            HTTP_LOG_DEBUG("[response]", "status={}", status);
        }
    }

    size_t current_buffer_size() const {
        return m_external_buffer != nullptr ? m_external_buffer_size : m_buffer.size();
    }

    void clear_external_buffer() {
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

using HttpWriter = HttpWriterImpl<AsyncTcpSocket>;

} // namespace galay::http

#ifdef GALAY_SSL_FEATURE_ENABLED
namespace galay::http {
using HttpsWriter = HttpWriterImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_WRITER_H
