#include <galay/cpp/galay-ws/builder/ws_frame_builder.h>

#include <galay/cpp/galay-ws/utils/ws_helper.h>

#include <utility>

namespace galay::websocket
{

WsFrameBuilder::WsFrameBuilder()
{
    m_frame.header.fin = true;
    m_frame.header.opcode = WsOpcode::Text;
    m_frame.header.mask = false;
}

WsFrameBuilder& WsFrameBuilder::opcode(WsOpcode opcode)
{
    m_frame.header.opcode = opcode;
    return *this;
}

WsFrameBuilder& WsFrameBuilder::fin(bool fin)
{
    m_frame.header.fin = fin;
    return *this;
}

WsFrameBuilder& WsFrameBuilder::payload(const std::string& payload)
{
    m_frame.payload = payload;
    m_frame.header.payload_length = m_frame.payload.size();
    return *this;
}

WsFrameBuilder& WsFrameBuilder::payload(std::string&& payload)
{
    m_frame.payload = std::move(payload);
    m_frame.header.payload_length = m_frame.payload.size();
    return *this;
}

WsFrameBuilder& WsFrameBuilder::text(const std::string& text, bool fin)
{
    return opcode(WsOpcode::Text).fin(fin).payload(text);
}

WsFrameBuilder& WsFrameBuilder::text(std::string&& text, bool fin)
{
    return opcode(WsOpcode::Text).fin(fin).payload(std::move(text));
}

WsFrameBuilder& WsFrameBuilder::binary(const std::string& data, bool fin)
{
    return opcode(WsOpcode::Binary).fin(fin).payload(data);
}

WsFrameBuilder& WsFrameBuilder::binary(std::string&& data, bool fin)
{
    return opcode(WsOpcode::Binary).fin(fin).payload(std::move(data));
}

WsFrameBuilder& WsFrameBuilder::ping(const std::string& data)
{
    return opcode(WsOpcode::Ping).fin(true).payload(data);
}

WsFrameBuilder& WsFrameBuilder::pong(const std::string& data)
{
    return opcode(WsOpcode::Pong).fin(true).payload(data);
}

WsFrameBuilder& WsFrameBuilder::close(WsCloseCode code, const std::string& reason)
{
    return opcode(WsOpcode::Close).fin(true).payload(buildWsClosePayload(code, reason));
}

WsFrame WsFrameBuilder::build() const
{
    return m_frame;
}

WsFrame WsFrameBuilder::buildMove()
{
    return std::move(m_frame);
}

WsFrameBuilder WsFrameBuilder::clone() const
{
    WsFrameBuilder copy;
    copy.m_frame = m_frame;
    return copy;
}

} // namespace galay::websocket
