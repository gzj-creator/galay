/**
 * @file ws_base.h
 * @brief WebSocket 协议基础类型定义
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 WebSocket 协议核心枚举（操作码、关闭状态码）、
 *          帧头结构与帧结构体，以及帧类型判断工具函数。
 */

#ifndef GALAY_WEBSOCKET_BASE_H
#define GALAY_WEBSOCKET_BASE_H

#include <cstdint>
#include <string>
#include <utility>

namespace galay::websocket
{

/**
 * @brief WebSocket 操作码
 * @details RFC 6455 定义的操作码
 */
enum class WsOpcode : uint8_t
{
    Continuation = 0x0,  ///< 继续帧
    Text = 0x1,          ///< 文本帧
    Binary = 0x2,        ///< 二进制帧
    Close = 0x8,         ///< 关闭帧
    Ping = 0x9,          ///< Ping 帧
    Pong = 0xA           ///< Pong 帧
};

/**
 * @brief WebSocket 关闭状态码
 * @details RFC 6455 定义的关闭状态码
 */
enum class WsCloseCode : uint16_t
{
    Normal = 1000,              ///< 正常关闭
    GoingAway = 1001,           ///< 端点离开
    ProtocolError = 1002,       ///< 协议错误
    UnsupportedData = 1003,     ///< 不支持的数据类型
    NoStatusReceived = 1005,    ///< 未收到状态码
    AbnormalClosure = 1006,     ///< 异常关闭
    InvalidPayload = 1007,      ///< 无效的 payload 数据
    PolicyViolation = 1008,     ///< 策略违规
    MessageTooBig = 1009,       ///< 消息过大
    MandatoryExtension = 1010,  ///< 强制扩展
    InternalError = 1011,       ///< 内部错误
    TlsHandshake = 1015         ///< TLS 握手失败
};

/**
 * @brief WebSocket 帧头结构
 * @details RFC 6455 定义的帧格式
 *
 * 帧格式:
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-------+-+-------------+-------------------------------+
 * |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
 * |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
 * |N|V|V|V|       |S|             |   (if payload len==126/127)   |
 * | |1|2|3|       |K|             |                               |
 * +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
 * |     Extended payload length continued, if payload len == 127  |
 * + - - - - - - - - - - - - - - - +-------------------------------+
 * |                               |Masking-key, if MASK set to 1  |
 * +-------------------------------+-------------------------------+
 * | Masking-key (continued)       |          Payload Data         |
 * +-------------------------------- - - - - - - - - - - - - - - - +
 * :                     Payload Data continued ...                :
 * + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
 * |                     Payload Data continued ...                |
 * +---------------------------------------------------------------+
 */
struct WsFrameHeader
{
    bool fin;                    ///< FIN 位：是否是最后一个分片
    bool rsv1;                   ///< RSV1 位：保留位1
    bool rsv2;                   ///< RSV2 位：保留位2
    bool rsv3;                   ///< RSV3 位：保留位3
    WsOpcode opcode;             ///< 操作码
    bool mask;                   ///< MASK 位：是否使用掩码
    uint64_t payload_length;     ///< Payload 长度
    uint8_t masking_key[4];      ///< 掩码密钥（如果 mask=1）

    WsFrameHeader()
        : fin(false)
        , rsv1(false)
        , rsv2(false)
        , rsv3(false)
        , opcode(WsOpcode::Text)
        , mask(false)
        , payload_length(0)
        , masking_key{0, 0, 0, 0}
    {
    }
};

/**
 * @brief WebSocket 帧
 */
struct WsFrame
{
    WsFrameHeader header;        ///< 帧头
    std::string payload;         ///< Payload 数据

    WsFrame() = default;

    WsFrame(WsOpcode opcode, const std::string& data, bool fin = true)
        : payload(data)
    {
        header.fin = fin;
        header.opcode = opcode;
        header.payload_length = data.size();
    }

    WsFrame(WsOpcode opcode, std::string&& data, bool fin = true)
        : payload(std::move(data))
    {
        header.fin = fin;
        header.opcode = opcode;
        header.payload_length = payload.size();
    }

    // 便捷的帧类型判断方法
    bool isPing() const { return header.opcode == WsOpcode::Ping; }
    bool isPong() const { return header.opcode == WsOpcode::Pong; }
    bool isClose() const { return header.opcode == WsOpcode::Close; }
    bool isText() const { return header.opcode == WsOpcode::Text; }
    bool isBinary() const { return header.opcode == WsOpcode::Binary; }
    bool isContinuation() const { return header.opcode == WsOpcode::Continuation; }

    bool isControlFrame() const {
        return header.opcode == WsOpcode::Close ||
               header.opcode == WsOpcode::Ping ||
               header.opcode == WsOpcode::Pong;
    }

    bool isDataFrame() const {
        return header.opcode == WsOpcode::Text ||
               header.opcode == WsOpcode::Binary ||
               header.opcode == WsOpcode::Continuation;
    }
};

/**
 * @brief 获取操作码名称
 */
inline const char* getOpcodeName(WsOpcode opcode)
{
    switch (opcode) {
        case WsOpcode::Continuation: return "Continuation";
        case WsOpcode::Text: return "Text";
        case WsOpcode::Binary: return "Binary";
        case WsOpcode::Close: return "Close";
        case WsOpcode::Ping: return "Ping";
        case WsOpcode::Pong: return "Pong";
        default: return "Unknown";
    }
}

/**
 * @brief 检查操作码是否是控制帧
 */
inline bool isControlFrame(WsOpcode opcode)
{
    return opcode == WsOpcode::Close ||
           opcode == WsOpcode::Ping ||
           opcode == WsOpcode::Pong;
}

/**
 * @brief 检查操作码是否是数据帧
 */
inline bool isDataFrame(WsOpcode opcode)
{
    return opcode == WsOpcode::Text ||
           opcode == WsOpcode::Binary ||
           opcode == WsOpcode::Continuation;
}

} // namespace galay::websocket

#endif // GALAY_WEBSOCKET_BASE_H
