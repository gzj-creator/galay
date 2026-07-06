#include "http2_frame.h"
#include <cstring>
#include <arpa/inet.h>

namespace galay::http2
{

// ==================== Http2FrameHeader ====================

void Http2FrameHeader::serialize(uint8_t* buffer) const
{
    // Length (24 bits, big-endian)
    buffer[0] = (length >> 16) & 0xFF;
    buffer[1] = (length >> 8) & 0xFF;
    buffer[2] = length & 0xFF;

    // Type (8 bits)
    buffer[3] = static_cast<uint8_t>(type);

    // Flags (8 bits)
    buffer[4] = flags;

    // Stream ID (31 bits, big-endian, R bit is 0)
    uint32_t sid = stream_id & 0x7FFFFFFF;
    buffer[5] = (sid >> 24) & 0xFF;
    buffer[6] = (sid >> 16) & 0xFF;
    buffer[7] = (sid >> 8) & 0xFF;
    buffer[8] = sid & 0xFF;
}

Http2FrameHeader Http2FrameHeader::deserialize(const uint8_t* buffer)
{
    Http2FrameHeader header;

    // Length (24 bits, big-endian)
    header.length = (static_cast<uint32_t>(buffer[0]) << 16) |
                    (static_cast<uint32_t>(buffer[1]) << 8) |
                    static_cast<uint32_t>(buffer[2]);

    // Type (8 bits)
    header.type = static_cast<Http2FrameType>(buffer[3]);

    // Flags (8 bits)
    header.flags = buffer[4];

    // Stream ID (31 bits, big-endian)
    header.stream_id = ((static_cast<uint32_t>(buffer[5]) << 24) |
                        (static_cast<uint32_t>(buffer[6]) << 16) |
                        (static_cast<uint32_t>(buffer[7]) << 8) |
                        static_cast<uint32_t>(buffer[8])) & 0x7FFFFFFF;

    return header;
}

// ==================== Http2DataFrame ====================

std::string Http2DataFrame::serialize() const
{
    std::string result;
    size_t payload_length = m_data.size();

    if (isPadded()) {
        payload_length += 1 + m_pad_length;
    }

    // 帧头
    Http2FrameHeader header = m_header;
    header.length = payload_length;

    result.resize(kHttp2FrameHeaderLength + payload_length);
    header.serialize(reinterpret_cast<uint8_t*>(result.data()));

    size_t offset = kHttp2FrameHeaderLength;

    // Pad Length (如果有)
    if (isPadded()) {
        result[offset++] = m_pad_length;
    }

    // Data
    std::memcpy(result.data() + offset, m_data.data(), m_data.size());
    offset += m_data.size();

    // Padding
    if (isPadded() && m_pad_length > 0) {
        std::memset(result.data() + offset, 0, m_pad_length);
    }

    return result;
}

Http2ErrorCode Http2DataFrame::parsePayload(const uint8_t* data, size_t length)
{
    size_t offset = 0;

    // Pad Length
    if (isPadded()) {
        if (length < 1) {
            return Http2ErrorCode::FrameSizeError;
        }
        m_pad_length = data[offset++];

        if (m_pad_length >= length) {
            return Http2ErrorCode::ProtocolError;
        }
    }

    // Data
    size_t data_length = length - offset - (isPadded() ? m_pad_length : 0);
    m_data.assign(reinterpret_cast<const char*>(data + offset), data_length);

    return Http2ErrorCode::NoError;
}

// ==================== Http2HeadersFrame ====================

std::string Http2HeadersFrame::serialize() const
{
    std::string result;
    size_t payload_length = m_header_block.size();

    if (hasPriority()) {
        payload_length += 5;  // E + Stream Dependency (4) + Weight (1)
    }

    if (isPadded()) {
        payload_length += 1 + m_pad_length;
    }

    Http2FrameHeader header = m_header;
    header.length = payload_length;

    result.resize(kHttp2FrameHeaderLength + payload_length);
    header.serialize(reinterpret_cast<uint8_t*>(result.data()));

    size_t offset = kHttp2FrameHeaderLength;

    // Pad Length
    if (isPadded()) {
        result[offset++] = m_pad_length;
    }

    // Priority
    if (hasPriority()) {
        uint32_t dep = m_stream_dependency;
        if (m_exclusive) {
            dep |= 0x80000000;
        }
        result[offset++] = (dep >> 24) & 0xFF;
        result[offset++] = (dep >> 16) & 0xFF;
        result[offset++] = (dep >> 8) & 0xFF;
        result[offset++] = dep & 0xFF;
        result[offset++] = m_weight;
    }

    // Header Block Fragment
    std::memcpy(result.data() + offset, m_header_block.data(), m_header_block.size());
    offset += m_header_block.size();

    // Padding
    if (isPadded() && m_pad_length > 0) {
        std::memset(result.data() + offset, 0, m_pad_length);
    }

    return result;
}

Http2ErrorCode Http2HeadersFrame::parsePayload(const uint8_t* data, size_t length)
{
    size_t offset = 0;

    // Pad Length
    if (isPadded()) {
        if (length < 1) {
            return Http2ErrorCode::FrameSizeError;
        }
        m_pad_length = data[offset++];
    }

    // Priority
    if (hasPriority()) {
        if (length - offset < 5) {
            return Http2ErrorCode::FrameSizeError;
        }

        uint32_t dep = (static_cast<uint32_t>(data[offset]) << 24) |
                       (static_cast<uint32_t>(data[offset + 1]) << 16) |
                       (static_cast<uint32_t>(data[offset + 2]) << 8) |
                       static_cast<uint32_t>(data[offset + 3]);

        m_exclusive = (dep & 0x80000000) != 0;
        m_stream_dependency = dep & 0x7FFFFFFF;
        offset += 4;

        m_weight = data[offset++];
    }

    // 检查 padding
    size_t padding = isPadded() ? m_pad_length : 0;
    if (offset + padding > length) {
        return Http2ErrorCode::ProtocolError;
    }

    // Header Block Fragment
    size_t block_length = length - offset - padding;
    m_header_block.assign(reinterpret_cast<const char*>(data + offset), block_length);

    return Http2ErrorCode::NoError;
}

// ==================== Http2PriorityFrame ====================

std::string Http2PriorityFrame::serialize() const
{
    std::string result;
    result.resize(kHttp2FrameHeaderLength + 5);

    Http2FrameHeader header = m_header;
    header.length = 5;
    header.serialize(reinterpret_cast<uint8_t*>(result.data()));

    size_t offset = kHttp2FrameHeaderLength;

    uint32_t dep = m_stream_dependency;
    if (m_exclusive) {
        dep |= 0x80000000;
    }

    result[offset++] = (dep >> 24) & 0xFF;
    result[offset++] = (dep >> 16) & 0xFF;
    result[offset++] = (dep >> 8) & 0xFF;
    result[offset++] = dep & 0xFF;
    result[offset++] = m_weight;

    return result;
}

Http2ErrorCode Http2PriorityFrame::parsePayload(const uint8_t* data, size_t length)
{
    if (length != 5) {
        return Http2ErrorCode::FrameSizeError;
    }

    uint32_t dep = (static_cast<uint32_t>(data[0]) << 24) |
                   (static_cast<uint32_t>(data[1]) << 16) |
                   (static_cast<uint32_t>(data[2]) << 8) |
                   static_cast<uint32_t>(data[3]);

    m_exclusive = (dep & 0x80000000) != 0;
    m_stream_dependency = dep & 0x7FFFFFFF;
    m_weight = data[4];

    return Http2ErrorCode::NoError;
}

// ==================== Http2RstStreamFrame ====================

std::string Http2RstStreamFrame::serialize() const
{
    std::string result;
    result.resize(kHttp2FrameHeaderLength + 4);

    Http2FrameHeader header = m_header;
    header.length = 4;
    header.serialize(reinterpret_cast<uint8_t*>(result.data()));

    uint32_t code = static_cast<uint32_t>(m_error_code);
    result[kHttp2FrameHeaderLength] = (code >> 24) & 0xFF;
    result[kHttp2FrameHeaderLength + 1] = (code >> 16) & 0xFF;
    result[kHttp2FrameHeaderLength + 2] = (code >> 8) & 0xFF;
    result[kHttp2FrameHeaderLength + 3] = code & 0xFF;

    return result;
}

Http2ErrorCode Http2RstStreamFrame::parsePayload(const uint8_t* data, size_t length)
{
    if (length != 4) {
        return Http2ErrorCode::FrameSizeError;
    }

    uint32_t code = (static_cast<uint32_t>(data[0]) << 24) |
                    (static_cast<uint32_t>(data[1]) << 16) |
                    (static_cast<uint32_t>(data[2]) << 8) |
                    static_cast<uint32_t>(data[3]);

    m_error_code = static_cast<Http2ErrorCode>(code);

    return Http2ErrorCode::NoError;
}

// ==================== Http2SettingsFrame ====================

std::string Http2SettingsFrame::serialize() const
{
    std::string result;

    if (isAck()) {
        // ACK 帧没有负载
        result.resize(kHttp2FrameHeaderLength);
        Http2FrameHeader header = m_header;
        header.length = 0;
        header.stream_id = 0;
        header.serialize(reinterpret_cast<uint8_t*>(result.data()));
    } else {
        size_t payload_length = m_settings.size() * 6;
        result.resize(kHttp2FrameHeaderLength + payload_length);

        Http2FrameHeader header = m_header;
        header.length = payload_length;
        header.stream_id = 0;
        header.serialize(reinterpret_cast<uint8_t*>(result.data()));

        size_t offset = kHttp2FrameHeaderLength;
        for (const auto& setting : m_settings) {
            uint16_t id = static_cast<uint16_t>(setting.id);
            result[offset++] = (id >> 8) & 0xFF;
            result[offset++] = id & 0xFF;

            result[offset++] = (setting.value >> 24) & 0xFF;
            result[offset++] = (setting.value >> 16) & 0xFF;
            result[offset++] = (setting.value >> 8) & 0xFF;
            result[offset++] = setting.value & 0xFF;
        }
    }

    return result;
}

Http2ErrorCode Http2SettingsFrame::parsePayload(const uint8_t* data, size_t length)
{
    // ACK 帧必须没有负载
    if (isAck()) {
        if (length != 0) {
            return Http2ErrorCode::FrameSizeError;
        }
        return Http2ErrorCode::NoError;
    }

    // 设置帧负载必须是 6 字节的倍数
    if (length % 6 != 0) {
        return Http2ErrorCode::FrameSizeError;
    }

    m_settings.clear();
    for (size_t i = 0; i < length; i += 6) {
        uint16_t id = (static_cast<uint16_t>(data[i]) << 8) |
                      static_cast<uint16_t>(data[i + 1]);

        uint32_t value = (static_cast<uint32_t>(data[i + 2]) << 24) |
                         (static_cast<uint32_t>(data[i + 3]) << 16) |
                         (static_cast<uint32_t>(data[i + 4]) << 8) |
                         static_cast<uint32_t>(data[i + 5]);

        m_settings.push_back(Http2SettingsFrame::Setting{
            .value = value,
            .id = static_cast<Http2SettingsId>(id)
        });
    }

    return Http2ErrorCode::NoError;
}

// ==================== Http2PushPromiseFrame ====================

std::string Http2PushPromiseFrame::serialize() const
{
    std::string result;
    size_t payload_length = 4 + m_header_block.size();  // Promised Stream ID + Header Block

    if (isPadded()) {
        payload_length += 1 + m_pad_length;
    }

    result.resize(kHttp2FrameHeaderLength + payload_length);

    Http2FrameHeader header = m_header;
    header.length = payload_length;
    header.serialize(reinterpret_cast<uint8_t*>(result.data()));

    size_t offset = kHttp2FrameHeaderLength;

    // Pad Length
    if (isPadded()) {
        result[offset++] = m_pad_length;
    }

    // Promised Stream ID (R bit is 0)
    uint32_t psid = m_promised_stream_id & 0x7FFFFFFF;
    result[offset++] = (psid >> 24) & 0xFF;
    result[offset++] = (psid >> 16) & 0xFF;
    result[offset++] = (psid >> 8) & 0xFF;
    result[offset++] = psid & 0xFF;

    // Header Block Fragment
    std::memcpy(result.data() + offset, m_header_block.data(), m_header_block.size());
    offset += m_header_block.size();

    // Padding
    if (isPadded() && m_pad_length > 0) {
        std::memset(result.data() + offset, 0, m_pad_length);
    }

    return result;
}

Http2ErrorCode Http2PushPromiseFrame::parsePayload(const uint8_t* data, size_t length)
{
    size_t offset = 0;

    // Pad Length
    if (isPadded()) {
        if (length < 1) {
            return Http2ErrorCode::FrameSizeError;
        }
        m_pad_length = data[offset++];
    }

    // Promised Stream ID
    if (length - offset < 4) {
        return Http2ErrorCode::FrameSizeError;
    }

    m_promised_stream_id = ((static_cast<uint32_t>(data[offset]) << 24) |
                            (static_cast<uint32_t>(data[offset + 1]) << 16) |
                            (static_cast<uint32_t>(data[offset + 2]) << 8) |
                            static_cast<uint32_t>(data[offset + 3])) & 0x7FFFFFFF;
    offset += 4;

    // 检查 padding
    size_t padding = isPadded() ? m_pad_length : 0;
    if (offset + padding > length) {
        return Http2ErrorCode::ProtocolError;
    }

    // Header Block Fragment
    size_t block_length = length - offset - padding;
    m_header_block.assign(reinterpret_cast<const char*>(data + offset), block_length);

    return Http2ErrorCode::NoError;
}

// ==================== Http2PingFrame ====================

std::string Http2PingFrame::serialize() const
{
    std::string result;
    result.resize(kHttp2FrameHeaderLength + 8);

    Http2FrameHeader header = m_header;
    header.length = 8;
    header.stream_id = 0;
    header.serialize(reinterpret_cast<uint8_t*>(result.data()));

    std::memcpy(result.data() + kHttp2FrameHeaderLength, m_opaque_data, 8);

    return result;
}

Http2ErrorCode Http2PingFrame::parsePayload(const uint8_t* data, size_t length)
{
    if (length != 8) {
        return Http2ErrorCode::FrameSizeError;
    }

    std::memcpy(m_opaque_data, data, 8);

    return Http2ErrorCode::NoError;
}

// ==================== Http2GoAwayFrame ====================

std::string Http2GoAwayFrame::serialize() const
{
    std::string result;
    size_t payload_length = 8 + m_debug_data.size();  // Last-Stream-ID + Error Code + Debug Data

    result.resize(kHttp2FrameHeaderLength + payload_length);

    Http2FrameHeader header = m_header;
    header.length = payload_length;
    header.stream_id = 0;
    header.serialize(reinterpret_cast<uint8_t*>(result.data()));

    size_t offset = kHttp2FrameHeaderLength;

    // Last-Stream-ID (R bit is 0)
    uint32_t lsid = m_last_stream_id & 0x7FFFFFFF;
    result[offset++] = (lsid >> 24) & 0xFF;
    result[offset++] = (lsid >> 16) & 0xFF;
    result[offset++] = (lsid >> 8) & 0xFF;
    result[offset++] = lsid & 0xFF;

    // Error Code
    uint32_t code = static_cast<uint32_t>(m_error_code);
    result[offset++] = (code >> 24) & 0xFF;
    result[offset++] = (code >> 16) & 0xFF;
    result[offset++] = (code >> 8) & 0xFF;
    result[offset++] = code & 0xFF;

    // Debug Data
    if (!m_debug_data.empty()) {
        std::memcpy(result.data() + offset, m_debug_data.data(), m_debug_data.size());
    }

    return result;
}

Http2ErrorCode Http2GoAwayFrame::parsePayload(const uint8_t* data, size_t length)
{
    if (length < 8) {
        return Http2ErrorCode::FrameSizeError;
    }

    // Last-Stream-ID
    m_last_stream_id = ((static_cast<uint32_t>(data[0]) << 24) |
                        (static_cast<uint32_t>(data[1]) << 16) |
                        (static_cast<uint32_t>(data[2]) << 8) |
                        static_cast<uint32_t>(data[3])) & 0x7FFFFFFF;

    // Error Code
    uint32_t code = (static_cast<uint32_t>(data[4]) << 24) |
                    (static_cast<uint32_t>(data[5]) << 16) |
                    (static_cast<uint32_t>(data[6]) << 8) |
                    static_cast<uint32_t>(data[7]);
    m_error_code = static_cast<Http2ErrorCode>(code);

    // Debug Data
    if (length > 8) {
        m_debug_data.assign(reinterpret_cast<const char*>(data + 8), length - 8);
    }

    return Http2ErrorCode::NoError;
}

// ==================== Http2WindowUpdateFrame ====================

std::string Http2WindowUpdateFrame::serialize() const
{
    std::string result;
    result.resize(kHttp2FrameHeaderLength + 4);

    Http2FrameHeader header = m_header;
    header.length = 4;
    header.serialize(reinterpret_cast<uint8_t*>(result.data()));

    // Window Size Increment (R bit is 0)
    uint32_t inc = m_window_size_increment & 0x7FFFFFFF;
    result[kHttp2FrameHeaderLength] = (inc >> 24) & 0xFF;
    result[kHttp2FrameHeaderLength + 1] = (inc >> 16) & 0xFF;
    result[kHttp2FrameHeaderLength + 2] = (inc >> 8) & 0xFF;
    result[kHttp2FrameHeaderLength + 3] = inc & 0xFF;

    return result;
}

Http2ErrorCode Http2WindowUpdateFrame::parsePayload(const uint8_t* data, size_t length)
{
    if (length != 4) {
        return Http2ErrorCode::FrameSizeError;
    }

    m_window_size_increment = ((static_cast<uint32_t>(data[0]) << 24) |
                               (static_cast<uint32_t>(data[1]) << 16) |
                               (static_cast<uint32_t>(data[2]) << 8) |
                               static_cast<uint32_t>(data[3])) & 0x7FFFFFFF;

    if (m_window_size_increment == 0) {
        return Http2ErrorCode::ProtocolError;
    }

    return Http2ErrorCode::NoError;
}

// ==================== Http2ContinuationFrame ====================

std::string Http2ContinuationFrame::serialize() const
{
    std::string result;
    result.resize(kHttp2FrameHeaderLength + m_header_block.size());

    Http2FrameHeader header = m_header;
    header.length = m_header_block.size();
    header.serialize(reinterpret_cast<uint8_t*>(result.data()));

    std::memcpy(result.data() + kHttp2FrameHeaderLength, m_header_block.data(), m_header_block.size());

    return result;
}

Http2ErrorCode Http2ContinuationFrame::parsePayload(const uint8_t* data, size_t length)
{
    m_header_block.assign(reinterpret_cast<const char*>(data), length);
    return Http2ErrorCode::NoError;
}

// ==================== Http2FrameParser ====================

Http2FrameHeader Http2FrameParser::parseHeader(const uint8_t* data)
{
    return Http2FrameHeader::deserialize(data);
}

Http2Frame::uptr Http2FrameParser::createFrame(Http2FrameType type)
{
    switch (type) {
        case Http2FrameType::Data:
            return std::make_unique<Http2DataFrame>();
        case Http2FrameType::Headers:
            return std::make_unique<Http2HeadersFrame>();
        case Http2FrameType::Priority:
            return std::make_unique<Http2PriorityFrame>();
        case Http2FrameType::RstStream:
            return std::make_unique<Http2RstStreamFrame>();
        case Http2FrameType::Settings:
            return std::make_unique<Http2SettingsFrame>();
        case Http2FrameType::PushPromise:
            return std::make_unique<Http2PushPromiseFrame>();
        case Http2FrameType::Ping:
            return std::make_unique<Http2PingFrame>();
        case Http2FrameType::GoAway:
            return std::make_unique<Http2GoAwayFrame>();
        case Http2FrameType::WindowUpdate:
            return std::make_unique<Http2WindowUpdateFrame>();
        case Http2FrameType::Continuation:
            return std::make_unique<Http2ContinuationFrame>();
        default:
            return nullptr;
    }
}

std::expected<Http2Frame::uptr, Http2ErrorCode> Http2FrameParser::parseFrame(const uint8_t* data, size_t length)
{
    if (length < kHttp2FrameHeaderLength) {
        return std::unexpected(Http2ErrorCode::FrameSizeError);
    }

    Http2FrameHeader header = parseHeader(data);

    if (length < kHttp2FrameHeaderLength + header.length) {
        return std::unexpected(Http2ErrorCode::FrameSizeError);
    }

    auto frame = createFrame(header.type);
    if (!frame) {
        // 未知帧类型，忽略
        return std::unexpected(Http2ErrorCode::ProtocolError);
    }

    frame->header() = header;

    Http2ErrorCode error = frame->parsePayload(data + kHttp2FrameHeaderLength, header.length);
    if (error != Http2ErrorCode::NoError) {
        return std::unexpected(error);
    }

    return frame;
}

std::string Http2FrameCodec::encode(const Http2Frame& frame)
{
    return frame.serialize();
}

std::expected<Http2Frame::uptr, Http2ErrorCode> Http2FrameCodec::decode(std::string_view bytes)
{
    return Http2FrameParser::parseFrame(
        reinterpret_cast<const uint8_t*>(bytes.data()),
        bytes.size());
}

} // namespace galay::http2
