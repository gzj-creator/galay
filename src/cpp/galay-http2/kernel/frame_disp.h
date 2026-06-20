/**
 * @file frame_disp.h
 * @brief HTTP/2 帧分发器，处理帧调度动作
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HTTP/2 连接级别的帧分发策略，
 *          将接收到的帧分发到对应的流处理器，
 *          并生成 GOAWAY、RST_STREAM 等协议动作。
 */

#ifndef GALAY_HTTP2_FRAME_DISPATCHER_H
#define GALAY_HTTP2_FRAME_DISPATCHER_H

#include "../protoc/http2_frame.h"
#include <unordered_map>
#include <vector>

namespace galay::http2
{

/**
 * @brief HTTP/2 帧分发动作类型
 */
enum class H2DispatchActionType
{
    DeliverToStream, ///< 分发给对应流处理器
    SendGoaway,      ///< 发送 GOAWAY 帧，关闭连接
    SendRstStream,   ///< 发送 RST_STREAM 帧，重置流
    AckSettings,     ///< 发送 SETTINGS ACK
    AckPing,         ///< 发送 PING ACK
    UpdateWindow,    ///< 更新连接或流窗口
    Ignore           ///< 忽略该帧或无需进一步动作
};

/**
 * @brief HTTP/2 分发错误作用域
 */
enum class H2DispatchErrorScope
{
    None,       ///< 无错误
    Connection, ///< 连接级错误，需要 GOAWAY
    Stream      ///< 流级错误，需要 RST_STREAM
};

struct H2DispatchAction
{
    H2DispatchActionType type = H2DispatchActionType::Ignore;
    uint32_t stream_id = 0;
    Http2ErrorCode error_code = Http2ErrorCode::NoError;
};

struct H2DispatchResult
{
    bool ok = true;
    H2DispatchErrorScope error_scope = H2DispatchErrorScope::None;
    Http2ErrorCode error_code = Http2ErrorCode::NoError;
    std::vector<H2DispatchAction> actions;
};

/**
 * @brief HTTP/2 stream 生命周期状态
 */
enum class H2StreamLifecycleState
{
    Idle,             ///< 尚未打开
    ReservedLocal,    ///< 本端保留，当前阶段暂不完整实现 PUSH_PROMISE
    ReservedRemote,   ///< 对端保留，当前阶段暂不完整实现 PUSH_PROMISE
    Open,             ///< 双向打开
    HalfClosedLocal,  ///< 本端已结束发送，对端仍可发送
    HalfClosedRemote, ///< 对端已结束发送，本端仍可发送
    Closed            ///< 流已关闭
};

/**
 * @brief 分发器维护的单 stream 状态
 */
struct H2DispatcherStreamState
{
    H2StreamLifecycleState lifecycle = H2StreamLifecycleState::Idle;
};

/**
 * @brief 分发器维护的连接级协议状态
 */
struct H2DispatcherConnectionState
{
    bool expecting_continuation = false;
    uint32_t continuation_stream_id = 0;
    bool goaway_received = false;
    uint32_t last_peer_stream_id = 0;
    uint32_t goaway_last_stream_id = 0;
    std::unordered_map<uint32_t, H2DispatcherStreamState> streams;
};

/**
 * @brief 连接级帧分发器（重写阶段最小状态机）
 */
class Http2FrameDispatcher
{
public:
    static H2DispatchResult dispatch(const Http2Frame& frame,
                                     H2DispatcherConnectionState& state);
};

} // namespace galay::http2

#endif // GALAY_HTTP2_FRAME_DISPATCHER_H
