/**
 * @file out_sched.h
 * @brief HTTP/2 出站调度器，管理发送队列和流量控制
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 Http2OutboundScheduler，负责 HTTP/2 出站帧的调度，
 *          管理连接级和流级的流量控制窗口，决定哪些流可以发送 DATA 帧。
 */

#ifndef GALAY_HTTP2_OUTBOUND_SCHEDULER_H
#define GALAY_HTTP2_OUTBOUND_SCHEDULER_H

#include "http2/protoc/http2_frame.h"
#include <cstdint>
#include <string>
#include <vector>

namespace galay::http2
{

/**
 * @brief HTTP/2 出站预算
 * @details 连接级的发送窗口和最大帧大小限制
 */
struct H2OutboundBudget
{
    int32_t conn_window = 0;                ///< 连接级流量控制窗口
    uint32_t max_frame_size = kDefaultMaxFrameSize; ///< 最大帧大小
};

/**
 * @brief HTTP/2 流发送状态
 * @details 单个流的发送窗口、待发送数据和优先级权重
 */
struct H2StreamSendState
{
    uint32_t stream_id = 0;                 ///< 流 ID
    int32_t stream_window = 0;              ///< 流级流量控制窗口
    std::string pending_data;               ///< 待发送的数据
    bool end_stream = false;                ///< 是否发送 END_STREAM 标志
    uint8_t weight = 16;                    ///< 流优先级权重
};

/**
 * @brief HTTP/2 出站调度选择结果
 * @details 包含本次调度选中的帧列表和总数据字节数
 */
struct H2OutboundSelection
{
    std::vector<Http2Frame::uptr> frames;   ///< 选中的帧列表
    size_t total_data_bytes = 0;            ///< 总数据字节数
};

/**
 * @brief 连接级出站调度器（重写阶段最小实现）
 */
class Http2OutboundScheduler
{
public:
    static H2OutboundSelection pickSendableFrames(H2OutboundBudget budget,
                                                  std::vector<H2StreamSendState>& streams);
};

} // namespace galay::http2

#endif // GALAY_HTTP2_OUTBOUND_SCHEDULER_H
