/**
 * @file http2_error.h
 * @brief HTTP/2 协议错误类型定义
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 HTTP/2 协议层面的错误类、运行时错误分类以及 GOAWAY 错误结构，
 *          用于区分连接级致命错误与流级可恢复错误。
 */

#ifndef GALAY_HTTP2_ERROR_H
#define GALAY_HTTP2_ERROR_H

#include "http2_base.h"
#include "galay-kernel/common/error.h"
#include <optional>
#include <string>
#include <expected>

namespace galay::http2
{

/**
 * @brief HTTP/2 错误类
 * @details 封装 HTTP/2 错误码与描述信息，提供静态工厂方法快速创建常见错误
 */
class Http2Error
{
public:
    Http2Error() = default;

    /**
     * @brief 构造 Http2Error
     * @param code 错误码
     * @param message 附加描述信息
     */
    Http2Error(Http2ErrorCode code, std::string message = "")
        : m_code(code), m_message(std::move(message)) {}

    /**
     * @brief 从底层 I/O 错误构造 HTTP/2 错误
     * @param io_error galay-kernel 返回的 I/O 错误
     * @details 供使用 Http2Error 作为 expected error_type 的状态机承载 I/O 错误，
     *          避免可恢复 I/O 错误落入内核 awaitable 的进程终止兜底路径。
     */
    explicit Http2Error(const galay::kernel::IOError& io_error)
        : m_code(Http2ErrorCode::InternalError)
        , m_message(io_error.message())
    {
        const uint64_t code = io_error.code();
        if (galay::kernel::IOError::contains(code, galay::kernel::kTimeout) ||
            galay::kernel::IOError::contains(code, galay::kernel::kDisconnectError) ||
            galay::kernel::IOError::contains(code, galay::kernel::kConnectFailed)) {
            m_code = Http2ErrorCode::ConnectError;
        }
    }

    Http2ErrorCode code() const { return m_code; } ///< 获取错误码
    const std::string& message() const { return m_message; } ///< 获取错误描述

    /**
     * @brief 判断是否存在错误
     * @return 非无错误时返回 true
     */
    bool isError() const { return m_code != Http2ErrorCode::NoError; }

    /**
     * @brief 转换为可读字符串
     * @return 格式为 "错误码名称: 附加信息"
     */
    std::string toString() const {
        return http2ErrorCodeToString(m_code) + (m_message.empty() ? "" : ": " + m_message);
    }

    static Http2Error noError() { return Http2Error(Http2ErrorCode::NoError); } ///< 创建无错误对象
    static Http2Error protocolError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::ProtocolError, msg); } ///< 创建协议错误
    static Http2Error internalError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::InternalError, msg); } ///< 创建内部错误
    static Http2Error flowControlError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::FlowControlError, msg); } ///< 创建流量控制错误
    static Http2Error frameSizeError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::FrameSizeError, msg); } ///< 创建帧大小错误
    static Http2Error compressionError(const std::string& msg = "") { return Http2Error(Http2ErrorCode::CompressionError, msg); } ///< 创建压缩错误

private:
    Http2ErrorCode m_code = Http2ErrorCode::NoError; ///< 错误码
    std::string m_message; ///< 附加描述信息
};

/**
 * @brief 连接/流运行时错误分类
 * @details 用于区分连接级致命错误与流级可恢复错误。
 */
enum class Http2RuntimeError
{
    ProtocolViolation,      ///< 协议违规
    FlowControlViolation,   ///< 流量控制违规
    StreamClosed,           ///< 流已关闭
    StreamReset,            ///< 流被重置
    Timeout,                ///< 超时
    PeerClosed,             ///< 对端关闭连接
    IoError                 ///< I/O 错误
};

/**
 * @brief 将运行时错误分类转换为字符串
 * @param error 运行时错误枚举
 * @return 错误名称字符串
 */
inline std::string http2RuntimeErrorToString(Http2RuntimeError error) {
    switch (error) {
        case Http2RuntimeError::ProtocolViolation:    return "protocol-violation";
        case Http2RuntimeError::FlowControlViolation: return "flow-control-violation";
        case Http2RuntimeError::StreamClosed:         return "stream-closed";
        case Http2RuntimeError::StreamReset:          return "stream-reset";
        case Http2RuntimeError::Timeout:              return "timeout";
        case Http2RuntimeError::PeerClosed:           return "peer-closed";
        case Http2RuntimeError::IoError:              return "io-error";
    }
    return "unknown";
}

/**
 * @brief 判断运行时错误是否为连接级致命错误
 * @param error 运行时错误枚举
 * @return 致命错误返回 true（ProtocolViolation/FlowControlViolation）
 */
inline bool http2IsConnectionFatal(Http2RuntimeError error) {
    switch (error) {
        case Http2RuntimeError::ProtocolViolation:
        case Http2RuntimeError::FlowControlViolation:
            return true;
        case Http2RuntimeError::StreamClosed:
        case Http2RuntimeError::StreamReset:
        case Http2RuntimeError::Timeout:
        case Http2RuntimeError::PeerClosed:
        case Http2RuntimeError::IoError:
            return false;
    }
    return false;
}

/**
 * @brief GOAWAY 导致的请求拒绝错误
 * @details retryable=true 表示该 stream 未被服务端接收，可安全重试
 */
struct Http2GoAwayError
{
    uint32_t stream_id = 0;                                ///< 当前流 ID
    uint32_t last_stream_id = 0;                           ///< GOAWAY 中最后处理的流 ID
    Http2ErrorCode error_code = Http2ErrorCode::NoError;   ///< GOAWAY 错误码
    bool retryable = false;                                ///< 是否可安全重试
    std::string debug;                                     ///< 调试信息

    std::string toString() const {
        return "GOAWAY stream_id=" + std::to_string(stream_id) +
               " last_stream_id=" + std::to_string(last_stream_id) +
               " error=" + http2ErrorCodeToString(error_code) +
               " retryable=" + std::string(retryable ? "true" : "false") +
               (debug.empty() ? "" : (" debug=" + debug));
    }
};

/**
 * @brief HTTP/2 core 边界错误
 * @details 用于在连接核心边界区分协议、流控、I/O 与超时错误，避免用异常表达可恢复错误。
 */
struct H2CoreError
{
    enum class Kind
    {
        Protocol,    ///< HTTP/2 协议错误
        FlowControl, ///< HTTP/2 流控错误
        Io,          ///< 底层 I/O 错误
        Timeout      ///< 定时器超时
    };

    Kind kind = Kind::Protocol;                               ///< 错误分类
    Http2ErrorCode h2_code = Http2ErrorCode::NoError;         ///< HTTP/2 错误码
    std::optional<galay::kernel::IOError> io_error;           ///< 可选 I/O 错误
};

} // namespace galay::http2

#endif // GALAY_HTTP2_ERROR_H
