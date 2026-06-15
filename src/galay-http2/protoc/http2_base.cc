#include "http2_base.h"

namespace galay::http2
{

std::string http2FrameTypeToString(Http2FrameType type)
{
    switch (type) {
        case Http2FrameType::Data: return "DATA";
        case Http2FrameType::Headers: return "HEADERS";
        case Http2FrameType::Priority: return "PRIORITY";
        case Http2FrameType::RstStream: return "RST_STREAM";
        case Http2FrameType::Settings: return "SETTINGS";
        case Http2FrameType::PushPromise: return "PUSH_PROMISE";
        case Http2FrameType::Ping: return "PING";
        case Http2FrameType::GoAway: return "GOAWAY";
        case Http2FrameType::WindowUpdate: return "WINDOW_UPDATE";
        case Http2FrameType::Continuation: return "CONTINUATION";
        default: return "UNKNOWN";
    }
}

std::string http2ErrorCodeToString(Http2ErrorCode code)
{
    switch (code) {
        case Http2ErrorCode::NoError: return "NO_ERROR";
        case Http2ErrorCode::ProtocolError: return "PROTOCOL_ERROR";
        case Http2ErrorCode::InternalError: return "INTERNAL_ERROR";
        case Http2ErrorCode::FlowControlError: return "FLOW_CONTROL_ERROR";
        case Http2ErrorCode::SettingsTimeout: return "SETTINGS_TIMEOUT";
        case Http2ErrorCode::StreamClosed: return "STREAM_CLOSED";
        case Http2ErrorCode::FrameSizeError: return "FRAME_SIZE_ERROR";
        case Http2ErrorCode::RefusedStream: return "REFUSED_STREAM";
        case Http2ErrorCode::Cancel: return "CANCEL";
        case Http2ErrorCode::CompressionError: return "COMPRESSION_ERROR";
        case Http2ErrorCode::ConnectError: return "CONNECT_ERROR";
        case Http2ErrorCode::EnhanceYourCalm: return "ENHANCE_YOUR_CALM";
        case Http2ErrorCode::InadequateSecurity: return "INADEQUATE_SECURITY";
        case Http2ErrorCode::Http11Required: return "HTTP_1_1_REQUIRED";
        default: return "UNKNOWN_ERROR";
    }
}

std::string http2StreamStateToString(Http2StreamState state)
{
    switch (state) {
        case Http2StreamState::Idle: return "idle";
        case Http2StreamState::ReservedLocal: return "reserved (local)";
        case Http2StreamState::ReservedRemote: return "reserved (remote)";
        case Http2StreamState::Open: return "open";
        case Http2StreamState::HalfClosedLocal: return "half-closed (local)";
        case Http2StreamState::HalfClosedRemote: return "half-closed (remote)";
        case Http2StreamState::Closed: return "closed";
        default: return "unknown";
    }
}

} // namespace galay::http2
