#ifndef GALAY_WS_READER_SETTING_H
#define GALAY_WS_READER_SETTING_H

#include <cstddef>

namespace galay::websocket
{

/**
 * @brief WebSocket读取器配置
 */
struct WsReaderSetting
{
    size_t max_frame_size;       ///< 单个帧的最大大小（默认 10MB）
    size_t max_message_size;     ///< 完整消息的最大大小（默认 100MB）
    bool auto_fragment;          ///< 是否自动处理分片消息（默认 true）

    WsReaderSetting()
        : max_frame_size(10 * 1024 * 1024)      // 10MB
        , max_message_size(100 * 1024 * 1024)   // 100MB
        , auto_fragment(true)
    {
    }
};

} // namespace galay::websocket

#endif // GALAY_WS_READER_SETTING_H
