/**
 * @file ws_frame_builder.h
 * @brief WebSocket frame builder helper
 * @author galay-http
 * @version 1.0.0
 */

#ifndef GALAY_WEBSOCKET_FRAME_BUILDER_H
#define GALAY_WEBSOCKET_FRAME_BUILDER_H

#include "galay-ws/protoc/ws_base.h"

#include <string>

namespace galay::websocket
{

/**
 * @brief WebSocket 帧构建器
 * @details 统一帧构建入口，避免热路径散落的临时对象拼装逻辑。
 */
class WsFrameBuilder
{
public:
    WsFrameBuilder();

    WsFrameBuilder& opcode(WsOpcode opcode);
    WsFrameBuilder& fin(bool fin = true);
    WsFrameBuilder& payload(const std::string& payload);
    WsFrameBuilder& payload(std::string&& payload);
    WsFrameBuilder& text(const std::string& text, bool fin = true);
    WsFrameBuilder& text(std::string&& text, bool fin = true);
    WsFrameBuilder& binary(const std::string& data, bool fin = true);
    WsFrameBuilder& binary(std::string&& data, bool fin = true);
    WsFrameBuilder& ping(const std::string& data = "");
    WsFrameBuilder& pong(const std::string& data = "");
    WsFrameBuilder& close(WsCloseCode code = WsCloseCode::Normal, const std::string& reason = "");

    WsFrame build() const;
    WsFrame buildMove();

private:
    WsFrame m_frame;
};

} // namespace galay::websocket

#endif // GALAY_WEBSOCKET_FRAME_BUILDER_H
