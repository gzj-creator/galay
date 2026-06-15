/**
 * @file ws_error.h
 * @brief WebSocket 协议错误码与错误类定义
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 WebSocket 协议层面所有可能的错误码，以及用于携带错误信息
 *          并可转换为关闭状态码的 WsError 类。
 */

#ifndef GALAY_WEBSOCKET_ERROR_H
#define GALAY_WEBSOCKET_ERROR_H

#include "ws_base.h"
#include "galay-kernel/common/error.h"
#include <string>

namespace galay::websocket
{

/**
 * @brief WebSocket 错误码
 */
enum WsErrorCode
{
    kWsNoError = 0,              ///< 无错误
    kWsIncomplete,               ///< 数据不完整
    kWsInvalidFrame,             ///< 无效的帧
    kWsInvalidOpcode,            ///< 无效的操作码
    kWsInvalidPayloadLength,     ///< 无效的 payload 长度
    kWsControlFrameTooLarge,     ///< 控制帧过大（>125字节）
    kWsControlFrameFragmented,   ///< 控制帧不能分片
    kWsInvalidUtf8,              ///< 无效的 UTF-8 编码
    kWsProtocolError,            ///< 协议错误
    kWsConnectionClosed,         ///< 连接已关闭
    kWsMessageTooLarge,          ///< 消息过大
    kWsInvalidCloseCode,         ///< 无效的关闭码
    kWsReservedBitsSet,          ///< 保留位被设置
    kWsMaskRequired,             ///< 需要掩码（客户端->服务器）
    kWsMaskNotAllowed,           ///< 不允许掩码（服务器->客户端）
    kWsConnectionError,          ///< 连接错误
    kWsSendError,                ///< 发送错误
    kWsUpgradeFailed,            ///< 升级失败
    kWsUnknownError              ///< 未知错误
};

/**
 * @brief WebSocket 错误类
 */
class WsError
{
public:
    /**
     * @brief 构造 WsError
     * @param code 错误码
     * @param extra_msg 附加错误描述
     */
    WsError(WsErrorCode code, const std::string& extra_msg = "")
        : m_code(code)
        , m_extra_msg(extra_msg)
    {
    }

    /**
     * @brief 从底层 I/O 错误构造 WebSocket 错误
     * @param io_error galay-kernel 返回的 I/O 错误
     * @details 让 WebSocket 状态机的 expected 结果可以承载底层 I/O 错误，
     *          避免可恢复 I/O 错误落入内核 awaitable 的进程终止兜底路径。
     */
    explicit WsError(const galay::kernel::IOError& io_error)
        : m_code(kWsConnectionError)
        , m_extra_msg(io_error.message())
    {
        const uint64_t code = io_error.code();
        if (galay::kernel::IOError::contains(code, galay::kernel::kDisconnectError)) {
            m_code = kWsConnectionClosed;
        } else if (galay::kernel::IOError::contains(code, galay::kernel::kSendFailed) ||
                   galay::kernel::IOError::contains(code, galay::kernel::kWriteFailed)) {
            m_code = kWsSendError;
        }
    }

    WsErrorCode code() const { return m_code; } ///< 获取错误码

    /**
     * @brief 获取错误描述信息
     * @return 包含标准描述与附加信息的字符串
     */
    std::string message() const
    {
        std::string msg = getErrorMessage(m_code);
        if (!m_extra_msg.empty()) {
            msg += ": " + m_extra_msg;
        }
        return msg;
    }

    /**
     * @brief 转换为 WebSocket 关闭状态码
     * @return 对应的 WsCloseCode 枚举值
     */
    WsCloseCode toCloseCode() const
    {
        switch (m_code) {
            case kWsInvalidFrame:
            case kWsInvalidOpcode:
            case kWsControlFrameTooLarge:
            case kWsControlFrameFragmented:
            case kWsReservedBitsSet:
            case kWsMaskRequired:
            case kWsMaskNotAllowed:
                return WsCloseCode::ProtocolError;

            case kWsInvalidUtf8:
            case kWsInvalidPayloadLength:
                return WsCloseCode::InvalidPayload;

            case kWsMessageTooLarge:
                return WsCloseCode::MessageTooBig;

            case kWsProtocolError:
                return WsCloseCode::ProtocolError;

            default:
                return WsCloseCode::InternalError;
        }
    }

private:
    static std::string getErrorMessage(WsErrorCode code)
    {
        switch (code) {
            case kWsNoError:
                return "No error";
            case kWsIncomplete:
                return "Data incomplete";
            case kWsInvalidFrame:
                return "Invalid frame";
            case kWsInvalidOpcode:
                return "Invalid opcode";
            case kWsInvalidPayloadLength:
                return "Invalid payload length";
            case kWsControlFrameTooLarge:
                return "Control frame too large (>125 bytes)";
            case kWsControlFrameFragmented:
                return "Control frame cannot be fragmented";
            case kWsInvalidUtf8:
                return "Invalid UTF-8 encoding";
            case kWsProtocolError:
                return "Protocol error";
            case kWsConnectionClosed:
                return "Connection closed";
            case kWsMessageTooLarge:
                return "Message too large";
            case kWsInvalidCloseCode:
                return "Invalid close code";
            case kWsReservedBitsSet:
                return "Reserved bits are set";
            case kWsMaskRequired:
                return "Mask required (client to server)";
            case kWsMaskNotAllowed:
                return "Mask not allowed (server to client)";
            case kWsConnectionError:
                return "Connection error";
            case kWsSendError:
                return "Send error";
            case kWsUpgradeFailed:
                return "WebSocket upgrade failed";
            case kWsUnknownError:
                return "Unknown error";
            default:
                return "Unknown error code";
        }
    }

private:
    WsErrorCode m_code;      ///< 错误码
    std::string m_extra_msg; ///< 附加错误描述
};

} // namespace galay::websocket

#endif // GALAY_WEBSOCKET_ERROR_H
