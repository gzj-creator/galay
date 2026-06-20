/**
 * @file flow_control.h
 * @brief HTTP/2 发送侧流量控制状态
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供连接级和 stream 级发送窗口的独立状态机。
 *          该类型只计算窗口与可发送字节数，不执行 socket 写入，也不阻塞。
 */

#ifndef GALAY_HTTP2_FLOW_CONTROL_H
#define GALAY_HTTP2_FLOW_CONTROL_H

#include "galay-http2/protoc/http2_base.h"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <unordered_map>

namespace galay::http2
{

constexpr int64_t kH2MaxFlowControlWindow = 0x7fffffff; ///< RFC 允许的最大窗口值

/**
 * @brief 发送窗口更新错误
 */
enum class H2FlowControlError
{
    WindowOverflow,    ///< 窗口更新超过 HTTP/2 最大窗口
    UnknownStream,     ///< 操作的 stream 尚未登记
    InsufficientWindow ///< 消耗窗口超过当前可用窗口
};

/**
 * @brief HTTP/2 发送窗口状态
 */
struct H2SendWindow
{
    int64_t conn_window = kDefaultInitialWindowSize;           ///< 连接级发送窗口
    int64_t initial_stream_window = kDefaultInitialWindowSize; ///< 新 stream 默认发送窗口
    std::unordered_map<uint32_t, int64_t> stream_windows;      ///< stream 级发送窗口
};

/**
 * @brief HTTP/2 发送侧流量控制器
 * @details 所有方法都是纯内存状态计算；不会阻塞线程，也不会发起 I/O。
 */
class H2FlowController
{
public:
    /**
     * @brief 确保 stream 窗口存在
     * @param stream_id stream ID
     * @return 成功或窗口错误
     */
    std::expected<void, H2FlowControlError> ensureStream(uint32_t stream_id);

    /**
     * @brief 应用连接级 WINDOW_UPDATE
     * @param increment 窗口增量
     * @return 成功或窗口溢出错误
     */
    std::expected<void, H2FlowControlError> applyConnectionWindowUpdate(uint32_t increment);

    /**
     * @brief 应用 stream 级 WINDOW_UPDATE
     * @param stream_id stream ID
     * @param increment 窗口增量
     * @return 成功、未知 stream 或窗口溢出错误
     */
    std::expected<void, H2FlowControlError> applyStreamWindowUpdate(uint32_t stream_id,
                                                                   uint32_t increment);

    /**
     * @brief 应用 SETTINGS_INITIAL_WINDOW_SIZE
     * @param new_size 新 stream 初始窗口
     * @return 成功或任一既有 stream 更新后溢出
     */
    std::expected<void, H2FlowControlError> applyInitialStreamWindowSize(uint32_t new_size);

    /**
     * @brief 计算当前可发送 DATA 字节数
     * @param stream_id stream ID
     * @param requested 调用方希望发送的字节数
     * @param max_frame_size 单帧最大载荷
     * @return 受连接窗口、stream 窗口、请求大小和最大帧大小共同限制的字节数
     */
    size_t availableToSend(uint32_t stream_id,
                           size_t requested,
                           uint32_t max_frame_size) const;

    /**
     * @brief 消耗连接级和 stream 级发送窗口
     * @param stream_id stream ID
     * @param bytes 已选择发送的 DATA 字节数
     * @return 成功、未知 stream 或窗口不足错误
     */
    std::expected<void, H2FlowControlError> consumeSendWindow(uint32_t stream_id,
                                                             size_t bytes);

private:
    H2SendWindow m_window;
};

} // namespace galay::http2

#endif // GALAY_HTTP2_FLOW_CONTROL_H
