/**
 * @file http2_frame.h
 * @brief HTTP/2 帧定义、解析器与编解码器
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 HTTP/2 所有帧类型（DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/
 *          PUSH_PROMISE/PING/GOAWAY/WINDOW_UPDATE/CONTINUATION），
 *          提供帧解析器及编解码统一入口。
 */

#ifndef GALAY_HTTP2_FRAME_H
#define GALAY_HTTP2_FRAME_H

#include "http2_base.h"
#include "http2_error.h"
#include <cstdint>
#include <cstring>
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <expected>
#include <optional>

namespace galay::http2
{

/**
 * @brief HTTP/2 帧头结构
 * @details 所有 HTTP/2 帧都以 9 字节的帧头开始
 */
struct Http2FrameHeader
{
    uint32_t length = 0;        // 帧负载长度 (24 bits)
    Http2FrameType type = Http2FrameType::Unknown;  // 帧类型 (8 bits)
    uint8_t flags = 0;          // 帧标志 (8 bits)
    uint32_t stream_id = 0;     // 流标识符 (31 bits, 最高位保留)

    // 序列化帧头到 9 字节
    void serialize(uint8_t* buffer) const;

    // 从 9 字节解析帧头
    static Http2FrameHeader deserialize(const uint8_t* buffer);

    // 检查标志位
    bool hasFlag(uint8_t flag) const { return (flags & flag) != 0; }
    void setFlag(uint8_t flag) { flags |= flag; }
    void clearFlag(uint8_t flag) { flags &= ~flag; }
};

// 前向声明所有帧子类（用于基类中的 asXXX 方法声明）
class Http2DataFrame;
class Http2HeadersFrame;
class Http2PriorityFrame;
class Http2RstStreamFrame;
class Http2SettingsFrame;
class Http2PushPromiseFrame;
class Http2PingFrame;
class Http2GoAwayFrame;
class Http2WindowUpdateFrame;
class Http2ContinuationFrame;

/**
 * @brief HTTP/2 帧基类
 */
class Http2Frame
{
public:
    using ptr = std::shared_ptr<Http2Frame>;
    using uptr = std::unique_ptr<Http2Frame>;

    Http2Frame() = default;
    explicit Http2Frame(const Http2FrameHeader& header) : m_header(header) {}
    virtual ~Http2Frame() = default;

    // 获取帧头
    Http2FrameHeader& header() { return m_header; }
    const Http2FrameHeader& header() const { return m_header; }

    // 获取帧类型
    Http2FrameType type() const { return m_header.type; }

    // 获取流 ID
    uint32_t streamId() const { return m_header.stream_id; }

    // 类型判断
    bool isData() const { return m_header.type == Http2FrameType::Data; }
    bool isHeaders() const { return m_header.type == Http2FrameType::Headers; }
    bool isPriority() const { return m_header.type == Http2FrameType::Priority; }
    bool isRstStream() const { return m_header.type == Http2FrameType::RstStream; }
    bool isSettings() const { return m_header.type == Http2FrameType::Settings; }
    bool isPushPromise() const { return m_header.type == Http2FrameType::PushPromise; }
    bool isPing() const { return m_header.type == Http2FrameType::Ping; }
    bool isGoAway() const { return m_header.type == Http2FrameType::GoAway; }
    bool isWindowUpdate() const { return m_header.type == Http2FrameType::WindowUpdate; }
    bool isContinuation() const { return m_header.type == Http2FrameType::Continuation; }

    // END_STREAM 判断（DATA 和 HEADERS 帧通用）
    bool isEndStream() const {
        return (m_header.type == Http2FrameType::Data || m_header.type == Http2FrameType::Headers)
            && m_header.hasFlag(Http2FrameFlags::kEndStream);
    }

    // 安全向下转型（定义在文件末尾，所有子类声明之后）
    inline Http2DataFrame* asData();
    inline Http2HeadersFrame* asHeaders();
    inline Http2PriorityFrame* asPriority();
    inline Http2RstStreamFrame* asRstStream();
    inline Http2SettingsFrame* asSettings();
    inline Http2PushPromiseFrame* asPushPromise();
    inline Http2PingFrame* asPing();
    inline Http2GoAwayFrame* asGoAway();
    inline Http2WindowUpdateFrame* asWindowUpdate();
    inline Http2ContinuationFrame* asContinuation();

    inline const Http2DataFrame* asData() const;
    inline const Http2HeadersFrame* asHeaders() const;
    inline const Http2PriorityFrame* asPriority() const;
    inline const Http2RstStreamFrame* asRstStream() const;
    inline const Http2SettingsFrame* asSettings() const;
    inline const Http2PushPromiseFrame* asPushPromise() const;
    inline const Http2PingFrame* asPing() const;
    inline const Http2GoAwayFrame* asGoAway() const;
    inline const Http2WindowUpdateFrame* asWindowUpdate() const;
    inline const Http2ContinuationFrame* asContinuation() const;

    // 序列化整个帧
    virtual std::string serialize() const = 0;

    // 从负载解析帧（帧头已解析）
    virtual Http2ErrorCode parsePayload(const uint8_t* data, size_t length) = 0;

protected:
    Http2FrameHeader m_header;
};

/**
 * @brief DATA 帧
 * @details 用于传输请求或响应的主体数据
 */
class Http2DataFrame : public Http2Frame
{
public:
    Http2DataFrame() { m_header.type = Http2FrameType::Data; }

    // 设置数据
    void setData(std::string data) { m_data = std::move(data); }
    void setData(const uint8_t* data, size_t length) { m_data.assign(reinterpret_cast<const char*>(data), length); }

    // 获取数据
    const std::string& data() const { return m_data; }
    std::string& data() { return m_data; }

    // END_STREAM 标志
    bool isEndStream() const { return m_header.hasFlag(Http2FrameFlags::kEndStream); }
    void setEndStream(bool value) {
        if (value) m_header.setFlag(Http2FrameFlags::kEndStream);
        else m_header.clearFlag(Http2FrameFlags::kEndStream);
    }

    // PADDED 标志
    bool isPadded() const { return m_header.hasFlag(Http2FrameFlags::kPadded); }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    std::string m_data;
    uint8_t m_pad_length = 0;
};

/**
 * @brief HEADERS 帧
 * @details 用于打开流并传输头部块片段
 */
class Http2HeadersFrame : public Http2Frame
{
public:
    Http2HeadersFrame() { m_header.type = Http2FrameType::Headers; }

    // 设置头部块片段
    void setHeaderBlock(std::string block) { m_header_block = std::move(block); }
    const std::string& headerBlock() const { return m_header_block; }
    std::string& headerBlock() { return m_header_block; }

    // END_STREAM 标志
    bool isEndStream() const { return m_header.hasFlag(Http2FrameFlags::kEndStream); }
    void setEndStream(bool value) {
        if (value) m_header.setFlag(Http2FrameFlags::kEndStream);
        else m_header.clearFlag(Http2FrameFlags::kEndStream);
    }

    // END_HEADERS 标志
    bool isEndHeaders() const { return m_header.hasFlag(Http2FrameFlags::kEndHeaders); }
    void setEndHeaders(bool value) {
        if (value) m_header.setFlag(Http2FrameFlags::kEndHeaders);
        else m_header.clearFlag(Http2FrameFlags::kEndHeaders);
    }

    // PRIORITY 标志
    bool hasPriority() const { return m_header.hasFlag(Http2FrameFlags::kPriority); }

    // PADDED 标志
    bool isPadded() const { return m_header.hasFlag(Http2FrameFlags::kPadded); }

    // 优先级字段
    bool exclusive() const { return m_exclusive; }
    uint32_t streamDependency() const { return m_stream_dependency; }
    uint8_t weight() const { return m_weight; }

    void setPriority(bool exclusive, uint32_t stream_dependency, uint8_t weight) {
        m_header.setFlag(Http2FrameFlags::kPriority);
        m_exclusive = exclusive;
        m_stream_dependency = stream_dependency;
        m_weight = weight;
    }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    std::string m_header_block;
    bool m_exclusive = false;
    uint32_t m_stream_dependency = 0;
    uint8_t m_weight = 16;  // 默认权重
    uint8_t m_pad_length = 0;
};

/**
 * @brief PRIORITY 帧
 * @details 用于指定流的优先级
 */
class Http2PriorityFrame : public Http2Frame
{
public:
    Http2PriorityFrame() { m_header.type = Http2FrameType::Priority; }

    bool exclusive() const { return m_exclusive; }
    uint32_t streamDependency() const { return m_stream_dependency; }
    uint8_t weight() const { return m_weight; }

    void setPriority(bool exclusive, uint32_t stream_dependency, uint8_t weight) {
        m_exclusive = exclusive;
        m_stream_dependency = stream_dependency;
        m_weight = weight;
    }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    bool m_exclusive = false;
    uint32_t m_stream_dependency = 0;
    uint8_t m_weight = 16;
};

/**
 * @brief RST_STREAM 帧
 * @details 用于立即终止流
 */
class Http2RstStreamFrame : public Http2Frame
{
public:
    Http2RstStreamFrame() { m_header.type = Http2FrameType::RstStream; }

    Http2ErrorCode errorCode() const { return m_error_code; }
    void setErrorCode(Http2ErrorCode code) { m_error_code = code; }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    Http2ErrorCode m_error_code = Http2ErrorCode::NoError;
};

/**
 * @brief SETTINGS 帧
 * @details 用于传输配置参数
 */
class Http2SettingsFrame : public Http2Frame
{
public:
    Http2SettingsFrame() { m_header.type = Http2FrameType::Settings; }

    // ACK 标志
    bool isAck() const { return m_header.hasFlag(Http2FrameFlags::kAck); }
    void setAck(bool value) {
        if (value) m_header.setFlag(Http2FrameFlags::kAck);
        else m_header.clearFlag(Http2FrameFlags::kAck);
    }

    // 设置参数
    struct Setting {
        Http2SettingsId id;
        uint32_t value;
    };

    void addSetting(Http2SettingsId id, uint32_t value) {
        m_settings.push_back({id, value});
    }

    const std::vector<Setting>& settings() const { return m_settings; }

    // 获取特定设置值
    std::optional<uint32_t> getSetting(Http2SettingsId id) const {
        for (const auto& s : m_settings) {
            if (s.id == id) return s.value;
        }
        return std::nullopt;
    }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    std::vector<Setting> m_settings;
};

/**
 * @brief PUSH_PROMISE 帧
 * @details 用于服务器推送（h2c 模式下通常禁用）
 */
class Http2PushPromiseFrame : public Http2Frame
{
public:
    Http2PushPromiseFrame() { m_header.type = Http2FrameType::PushPromise; }

    uint32_t promisedStreamId() const { return m_promised_stream_id; }
    void setPromisedStreamId(uint32_t id) { m_promised_stream_id = id; }

    const std::string& headerBlock() const { return m_header_block; }
    void setHeaderBlock(std::string block) { m_header_block = std::move(block); }

    bool isEndHeaders() const { return m_header.hasFlag(Http2FrameFlags::kEndHeaders); }
    void setEndHeaders(bool value) {
        if (value) m_header.setFlag(Http2FrameFlags::kEndHeaders);
        else m_header.clearFlag(Http2FrameFlags::kEndHeaders);
    }

    // PADDED 标志
    bool isPadded() const { return m_header.hasFlag(Http2FrameFlags::kPadded); }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    uint32_t m_promised_stream_id = 0;
    std::string m_header_block;
    uint8_t m_pad_length = 0;
};

/**
 * @brief PING 帧
 * @details 用于测量往返时间和保持连接活跃
 */
class Http2PingFrame : public Http2Frame
{
public:
    Http2PingFrame() {
        m_header.type = Http2FrameType::Ping;
        m_header.stream_id = 0;  // PING 帧必须在流 0 上
    }

    // ACK 标志
    bool isAck() const { return m_header.hasFlag(Http2FrameFlags::kAck); }
    void setAck(bool value) {
        if (value) m_header.setFlag(Http2FrameFlags::kAck);
        else m_header.clearFlag(Http2FrameFlags::kAck);
    }

    // 8 字节不透明数据
    const uint8_t* opaqueData() const { return m_opaque_data; }
    void setOpaqueData(const uint8_t* data) {
        std::memcpy(m_opaque_data, data, 8);
    }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    uint8_t m_opaque_data[8] = {0};
};

/**
 * @brief GOAWAY 帧
 * @details 用于启动连接关闭或发出严重错误信号
 */
class Http2GoAwayFrame : public Http2Frame
{
public:
    Http2GoAwayFrame() {
        m_header.type = Http2FrameType::GoAway;
        m_header.stream_id = 0;  // GOAWAY 帧必须在流 0 上
    }

    uint32_t lastStreamId() const { return m_last_stream_id; }
    void setLastStreamId(uint32_t id) { m_last_stream_id = id; }

    Http2ErrorCode errorCode() const { return m_error_code; }
    void setErrorCode(Http2ErrorCode code) { m_error_code = code; }

    const std::string& debugData() const { return m_debug_data; }
    void setDebugData(std::string data) { m_debug_data = std::move(data); }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    uint32_t m_last_stream_id = 0;
    Http2ErrorCode m_error_code = Http2ErrorCode::NoError;
    std::string m_debug_data;
};

/**
 * @brief WINDOW_UPDATE 帧
 * @details 用于流量控制
 */
class Http2WindowUpdateFrame : public Http2Frame
{
public:
    Http2WindowUpdateFrame() { m_header.type = Http2FrameType::WindowUpdate; }

    uint32_t windowSizeIncrement() const { return m_window_size_increment; }
    void setWindowSizeIncrement(uint32_t increment) { m_window_size_increment = increment; }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    uint32_t m_window_size_increment = 0;
};

/**
 * @brief CONTINUATION 帧
 * @details 用于继续传输头部块片段
 */
class Http2ContinuationFrame : public Http2Frame
{
public:
    Http2ContinuationFrame() { m_header.type = Http2FrameType::Continuation; }

    const std::string& headerBlock() const { return m_header_block; }
    void setHeaderBlock(std::string block) { m_header_block = std::move(block); }

    bool isEndHeaders() const { return m_header.hasFlag(Http2FrameFlags::kEndHeaders); }
    void setEndHeaders(bool value) {
        if (value) m_header.setFlag(Http2FrameFlags::kEndHeaders);
        else m_header.clearFlag(Http2FrameFlags::kEndHeaders);
    }

    std::string serialize() const override;
    Http2ErrorCode parsePayload(const uint8_t* data, size_t length) override;

private:
    std::string m_header_block;
};

// ==================== Http2Frame::asXXX 内联定义 ====================

inline Http2DataFrame* Http2Frame::asData() { return isData() ? static_cast<Http2DataFrame*>(this) : nullptr; }
inline Http2HeadersFrame* Http2Frame::asHeaders() { return isHeaders() ? static_cast<Http2HeadersFrame*>(this) : nullptr; }
inline Http2PriorityFrame* Http2Frame::asPriority() { return isPriority() ? static_cast<Http2PriorityFrame*>(this) : nullptr; }
inline Http2RstStreamFrame* Http2Frame::asRstStream() { return isRstStream() ? static_cast<Http2RstStreamFrame*>(this) : nullptr; }
inline Http2SettingsFrame* Http2Frame::asSettings() { return isSettings() ? static_cast<Http2SettingsFrame*>(this) : nullptr; }
inline Http2PushPromiseFrame* Http2Frame::asPushPromise() { return isPushPromise() ? static_cast<Http2PushPromiseFrame*>(this) : nullptr; }
inline Http2PingFrame* Http2Frame::asPing() { return isPing() ? static_cast<Http2PingFrame*>(this) : nullptr; }
inline Http2GoAwayFrame* Http2Frame::asGoAway() { return isGoAway() ? static_cast<Http2GoAwayFrame*>(this) : nullptr; }
inline Http2WindowUpdateFrame* Http2Frame::asWindowUpdate() { return isWindowUpdate() ? static_cast<Http2WindowUpdateFrame*>(this) : nullptr; }
inline Http2ContinuationFrame* Http2Frame::asContinuation() { return isContinuation() ? static_cast<Http2ContinuationFrame*>(this) : nullptr; }

inline const Http2DataFrame* Http2Frame::asData() const { return isData() ? static_cast<const Http2DataFrame*>(this) : nullptr; }
inline const Http2HeadersFrame* Http2Frame::asHeaders() const { return isHeaders() ? static_cast<const Http2HeadersFrame*>(this) : nullptr; }
inline const Http2PriorityFrame* Http2Frame::asPriority() const { return isPriority() ? static_cast<const Http2PriorityFrame*>(this) : nullptr; }
inline const Http2RstStreamFrame* Http2Frame::asRstStream() const { return isRstStream() ? static_cast<const Http2RstStreamFrame*>(this) : nullptr; }
inline const Http2SettingsFrame* Http2Frame::asSettings() const { return isSettings() ? static_cast<const Http2SettingsFrame*>(this) : nullptr; }
inline const Http2PushPromiseFrame* Http2Frame::asPushPromise() const { return isPushPromise() ? static_cast<const Http2PushPromiseFrame*>(this) : nullptr; }
inline const Http2PingFrame* Http2Frame::asPing() const { return isPing() ? static_cast<const Http2PingFrame*>(this) : nullptr; }
inline const Http2GoAwayFrame* Http2Frame::asGoAway() const { return isGoAway() ? static_cast<const Http2GoAwayFrame*>(this) : nullptr; }
inline const Http2WindowUpdateFrame* Http2Frame::asWindowUpdate() const { return isWindowUpdate() ? static_cast<const Http2WindowUpdateFrame*>(this) : nullptr; }
inline const Http2ContinuationFrame* Http2Frame::asContinuation() const { return isContinuation() ? static_cast<const Http2ContinuationFrame*>(this) : nullptr; }

/**
 * @brief HTTP/2 帧解析器
 */
class Http2FrameParser
{
public:
    /**
     * @brief 解析帧头
     * @param data 数据指针（至少 9 字节）
     * @return 帧头
     */
    static Http2FrameHeader parseHeader(const uint8_t* data);

    /**
     * @brief 解析完整帧
     * @param data 数据指针
     * @param length 数据长度
     * @return 解析结果：帧指针或错误码
     */
    static std::expected<Http2Frame::uptr, Http2ErrorCode> parseFrame(const uint8_t* data, size_t length);

    /**
     * @brief 根据帧类型创建帧对象
     * @param type 帧类型
     * @return 帧对象
     */
    static Http2Frame::uptr createFrame(Http2FrameType type);
};

/**
 * @brief 帧编解码统一入口
 * @details 对上层暴露稳定接口，内部复用 Http2FrameParser 与各帧 serialize。
 */
class Http2FrameCodec
{
public:
    static std::string encode(const Http2Frame& frame);
    static std::expected<Http2Frame::uptr, Http2ErrorCode> decode(std::string_view bytes);
};

} // namespace galay::http2

#include "../builder/http2_frame_builder.h"

#endif // GALAY_HTTP2_FRAME_H
