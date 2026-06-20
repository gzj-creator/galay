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

#include "galay-http2/protoc/http2_frame.h"
#include <cstddef>
#include <cstdint>
#include <deque>
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
 * @brief HTTP/2 待发送 DATA 缓冲
 * @details chunks 保存原始数据块，front_offset 表示首块已发送偏移，
 *          避免通过字符串头部 erase 搬移剩余数据。
 */
struct H2PendingData
{
    std::deque<std::string> chunks;         ///< 待发送数据块
    size_t front_offset = 0;                ///< 首块已发送偏移
    bool end_stream = false;                ///< 数据发送完后是否发送 END_STREAM
};

/**
 * @brief HTTP/2 流发送状态
 * @details 单个流的发送窗口、待发送数据和优先级权重
 */
struct H2StreamSendState
{
    uint32_t stream_id = 0;                 ///< 流 ID
    int32_t stream_window = 0;              ///< 流级流量控制窗口
    H2PendingData pending;                  ///< 待发送 DATA 缓冲
    uint8_t weight = 16;                    ///< 流优先级权重
    size_t deficit = 0;                     ///< DRR 当前可用发送额度
    bool queued = false;                    ///< 是否已经进入调度轮转
};

/**
 * @brief HTTP/2 出站调度配置
 */
struct H2SchedulerConfig
{
    size_t base_quantum = 16 * 1024;        ///< DRR 基础 quantum，实际额度乘以 stream weight
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
 * @brief HTTP/2 出站 bytes 调度选择结果
 * @details 用于热路径直接产出已序列化 DATA 帧，减少 Http2DataFrame 对象分配。
 */
struct H2OutboundBytesSelection
{
    std::vector<std::string> frames;        ///< 已序列化的 HTTP/2 帧字节
    size_t total_data_bytes = 0;            ///< 总 DATA payload 字节数
};

/**
 * @brief HTTP/2 出站队列
 * @details 控制帧和 HEADERS 不受 DATA flow control 阻塞，DATA 由 data_streams 调度。
 */
struct H2OutboundQueues
{
    std::deque<Http2Frame::uptr> control_frames; ///< SETTINGS/PING/RST/GOAWAY 等控制帧
    std::deque<Http2Frame::uptr> header_frames;  ///< HEADERS 帧
    std::vector<H2StreamSendState> data_streams; ///< DATA stream 队列
};

/**
 * @brief 连接级出站调度器（重写阶段最小实现）
 */
class Http2OutboundScheduler
{
public:
    static H2OutboundSelection pickSendableFrames(H2OutboundBudget budget,
                                                  std::vector<H2StreamSendState>& streams,
                                                  H2SchedulerConfig config = {});

    static H2OutboundSelection pickSendableFrames(H2OutboundBudget budget,
                                                  H2OutboundQueues& queues,
                                                  H2SchedulerConfig config = {});

    static H2OutboundBytesSelection pickSendableBytes(H2OutboundBudget budget,
                                                      H2OutboundQueues& queues,
                                                      H2SchedulerConfig config = {});

    static H2OutboundBytesSelection pickSendableBytes(H2OutboundBudget budget,
                                                      std::vector<H2StreamSendState>& streams,
                                                      H2SchedulerConfig config = {});
};

} // namespace galay::http2

#endif // GALAY_HTTP2_OUTBOUND_SCHEDULER_H
