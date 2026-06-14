/**
 * @file http2_base.h
 * @brief HTTP/2 协议基础常量、枚举与工具函数
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 HTTP/2 协议的核心常量（连接前言、帧头长度、默认设置），
 *          帧类型、帧标志、设置参数、错误码、流状态等枚举，
 *          以及枚举值转字符串的工具函数。
 */

#ifndef GALAY_HTTP2_BASE_H
#define GALAY_HTTP2_BASE_H

#include <cstdint>
#include <string>
#include <string_view>

namespace galay::http2
{

// HTTP/2 连接前言 (Connection Preface)
// 客户端必须在连接开始时发送此字符串
constexpr std::string_view kHttp2ConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"; ///< HTTP/2 客户端连接前言
constexpr size_t kHttp2ConnectionPrefaceLength = 24; ///< 连接前言长度

constexpr size_t kHttp2FrameHeaderLength = 9; ///< HTTP/2 帧头长度固定为 9 字节

// HTTP/2 默认设置
constexpr uint32_t kDefaultHeaderTableSize = 4096;       ///< 默认 HPACK 动态表大小
constexpr uint32_t kDefaultEnablePush = 1;               ///< 默认启用服务器推送
constexpr uint32_t kDefaultMaxConcurrentStreams = 100;    ///< 默认最大并发流数
constexpr uint32_t kDefaultInitialWindowSize = 65535;    ///< 默认初始窗口大小
constexpr uint32_t kDefaultMaxFrameSize = 16384;         ///< 默认最大帧大小
constexpr uint32_t kDefaultMaxHeaderListSize = 8192;     ///< 默认最大头部列表大小

// HTTP/2 帧大小限制
constexpr uint32_t kMinFrameSize = 16384;      ///< 最小帧大小（2^14）
constexpr uint32_t kMaxFrameSize = 16777215;   ///< 最大帧大小（2^24 - 1）

constexpr uint32_t kMaxStreamId = 0x7FFFFFFF;  ///< 最大流 ID（2^31 - 1）

/**
 * @brief HTTP/2 帧类型枚举
 * @details 定义 RFC 7540 中所有标准帧类型
 */
enum class Http2FrameType : uint8_t
{
    Data = 0x0,            ///< DATA 帧，传输数据
    Headers = 0x1,         ///< HEADERS 帧，传输头部
    Priority = 0x2,        ///< PRIORITY 帧，指定优先级
    RstStream = 0x3,       ///< RST_STREAM 帧，终止流
    Settings = 0x4,        ///< SETTINGS 帧，传输配置
    PushPromise = 0x5,     ///< PUSH_PROMISE 帧，服务器推送
    Ping = 0x6,            ///< PING 帧，保活与 RTT 测量
    GoAway = 0x7,          ///< GOAWAY 帧，发起连接关闭
    WindowUpdate = 0x8,    ///< WINDOW_UPDATE 帧，流量控制
    Continuation = 0x9,    ///< CONTINUATION 帧，继续传输头部块
    Unknown = 0xFF         ///< 未知帧类型
};

/**
 * @brief HTTP/2 帧标志常量
 */
namespace Http2FrameFlags
{
    constexpr uint8_t kEndStream = 0x1;   ///< END_STREAM 标志（DATA/HEADERS）
    constexpr uint8_t kPadded = 0x8;      ///< PADDED 标志（DATA/HEADERS/PUSH_PROMISE）

    constexpr uint8_t kEndHeaders = 0x4;  ///< END_HEADERS 标志（HEADERS/PUSH_PROMISE/CONTINUATION）
    constexpr uint8_t kPriority = 0x20;   ///< PRIORITY 标志（HEADERS）

    constexpr uint8_t kAck = 0x1;         ///< ACK 标志（SETTINGS/PING）
}

/**
 * @brief HTTP/2 设置参数标识符
 */
enum class Http2SettingsId : uint16_t
{
    HeaderTableSize = 0x1,         ///< HPACK 动态表大小
    EnablePush = 0x2,              ///< 是否启用服务器推送
    MaxConcurrentStreams = 0x3,    ///< 最大并发流数
    InitialWindowSize = 0x4,      ///< 初始窗口大小
    MaxFrameSize = 0x5,           ///< 最大帧大小
    MaxHeaderListSize = 0x6       ///< 最大头部列表大小
};

/**
 * @brief HTTP/2 错误码枚举
 * @details 定义 RFC 7540 中所有标准错误码
 */
enum class Http2ErrorCode : uint32_t
{
    NoError = 0x0,               ///< 无错误
    ProtocolError = 0x1,         ///< 协议错误
    InternalError = 0x2,         ///< 内部错误
    FlowControlError = 0x3,      ///< 流量控制错误
    SettingsTimeout = 0x4,       ///< 设置超时
    StreamClosed = 0x5,          ///< 流已关闭
    FrameSizeError = 0x6,        ///< 帧大小错误
    RefusedStream = 0x7,         ///< 流被拒绝
    Cancel = 0x8,                ///< 取消
    CompressionError = 0x9,      ///< 压缩错误
    ConnectError = 0xa,          ///< 连接错误
    EnhanceYourCalm = 0xb,       ///< 需要降低速率
    InadequateSecurity = 0xc,    ///< 安全级别不足
    Http11Required = 0xd         ///< 需要 HTTP/1.1
};

/**
 * @brief HTTP/2 流状态枚举
 * @details 定义 RFC 7540 中流的生命周期状态
 */
enum class Http2StreamState
{
    Idle,               ///< 空闲
    ReservedLocal,      ///< 本地保留
    ReservedRemote,     ///< 远端保留
    Open,               ///< 打开
    HalfClosedLocal,    ///< 本端半关闭
    HalfClosedRemote,   ///< 远端半关闭
    Closed              ///< 关闭
};

/**
 * @brief 将帧类型转换为字符串
 * @param type 帧类型枚举
 * @return 帧类型名称字符串
 */
std::string http2FrameTypeToString(Http2FrameType type);

/**
 * @brief 将错误码转换为字符串
 * @param code 错误码枚举
 * @return 错误码名称字符串
 */
std::string http2ErrorCodeToString(Http2ErrorCode code);

/**
 * @brief 将流状态转换为字符串
 * @param state 流状态枚举
 * @return 流状态名称字符串
 */
std::string http2StreamStateToString(Http2StreamState state);

} // namespace galay::http2

#endif // GALAY_HTTP2_BASE_H
