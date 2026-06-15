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

#include "galay-http2/protoc/http2_frame.h"
#include <vector>

namespace galay::http2
{

/**
 * @brief HTTP/2 帧分发动作类型
 */
enum class H2DispatchActionType
{
    SendGoaway,     ///< 发送 GOAWAY 帧，关闭连接
    SendRstStream   ///< 发送 RST_STREAM 帧，重置流
};

struct H2DispatchAction
{
    H2DispatchActionType type;
    uint32_t stream_id = 0;
    Http2ErrorCode error_code = Http2ErrorCode::NoError;
};

struct H2DispatchResult
{
    bool ok = true;
    std::vector<H2DispatchAction> actions;
};

struct H2DispatcherConnectionState
{
    bool expecting_continuation = false;
    uint32_t continuation_stream_id = 0;
    bool goaway_received = false;
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
