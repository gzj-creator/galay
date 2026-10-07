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
#include <utility>

namespace galay::http2
{

/**
 * @brief HTTP/2 帧头结构
 * @details 所有 HTTP/2 帧都以 9 字节的帧头开始
 */
struct Http2FrameHeader
{
    uint32_t length = 0;        // 帧负载长度 (24 bits)
    uint32_t stream_id = 0;     // 流标识符 (31 bits, 最高位保留)
    Http2FrameType type = Http2FrameType::Unknown;  // 帧类型 (8 bits)
    uint8_t flags = 0;          // 帧标志 (8 bits)

    // 序列化帧头到 9 字节
    void serialize(uint8_t* buffer) const;

    // 从 9 字节解析帧头
    static Http2FrameHeader deserialize(const uint8_t* buffer);

    // 检查标志位
    bool has_flag(uint8_t flag) const { return (flags & flag) != 0; }
    void set_flag(uint8_t flag) { flags |= flag; }
    void clear_flag(uint8_t flag) { flags &= ~flag; }
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
    uint32_t stream_id() const { return m_header.stream_id; }

    // 类型判断
    bool is_data() const { return m_header.type == Http2FrameType::Data; }
    bool is_headers() const { return m_header.type == Http2FrameType::Headers; }
    bool is_priority() const { return m_header.type == Http2FrameType::Priority; }
    bool is_rst_stream() const { return m_header.type == Http2FrameType::RstStream; }
    bool is_settings() const { return m_header.type == Http2FrameType::Settings; }
    bool is_push_promise() const { return m_header.type == Http2FrameType::PushPromise; }
    bool is_ping() const { return m_header.type == Http2FrameType::Ping; }
    bool is_go_away() const { return m_header.type == Http2FrameType::GoAway; }
    bool is_window_update() const { return m_header.type == Http2FrameType::WindowUpdate; }
    bool is_continuation() const { return m_header.type == Http2FrameType::Continuation; }

    // END_STREAM 判断（DATA 和 HEADERS 帧通用）
    bool is_end_stream() const {
        return (m_header.type == Http2FrameType::Data || m_header.type == Http2FrameType::Headers)
            && m_header.has_flag(Http2FrameFlags::kEndStream);
    }

    // 安全向下转型（定义在文件末尾，所有子类声明之后）
    inline Http2DataFrame* as_data();
    inline Http2HeadersFrame* as_headers();
    inline Http2PriorityFrame* as_priority();
    inline Http2RstStreamFrame* as_rst_stream();
    inline Http2SettingsFrame* as_settings();
    inline Http2PushPromiseFrame* as_push_promise();
    inline Http2PingFrame* as_ping();
    inline Http2GoAwayFrame* as_go_away();
    inline Http2WindowUpdateFrame* as_window_update();
    inline Http2ContinuationFrame* as_continuation();

    inline const Http2DataFrame* as_data() const;
    inline const Http2HeadersFrame* as_headers() const;
    inline const Http2PriorityFrame* as_priority() const;
    inline const Http2RstStreamFrame* as_rst_stream() const;
    inline const Http2SettingsFrame* as_settings() const;
    inline const Http2PushPromiseFrame* as_push_promise() const;
    inline const Http2PingFrame* as_ping() const;
    inline const Http2GoAwayFrame* as_go_away() const;
    inline const Http2WindowUpdateFrame* as_window_update() const;
    inline const Http2ContinuationFrame* as_continuation() const;

    // 序列化整个帧
    virtual std::string serialize() const = 0;

    // 从负载解析帧（帧头已解析）
    virtual Http2ErrorCode parse_payload(const uint8_t* data, size_t length) = 0;

protected:
    Http2Frame(Http2Frame&&) noexcept = default;
    Http2Frame& operator=(Http2Frame&&) noexcept = default;

    Http2FrameHeader m_header;

private:
    Http2Frame(const Http2Frame&) = delete;
    Http2Frame& operator=(const Http2Frame&) = delete;
};

/**
 * @brief DATA 帧
 * @details 用于传输请求或响应的主体数据
 */
class Http2DataFrame : public Http2Frame
{
public:
    Http2DataFrame() { m_header.type = Http2FrameType::Data; }
    Http2DataFrame(Http2DataFrame&&) noexcept = default;
    Http2DataFrame& operator=(Http2DataFrame&&) noexcept = default;

    // 设置数据
    void set_data(std::string data) { m_data = std::move(data); }
    void set_data(const uint8_t* data, size_t length) { m_data.assign(reinterpret_cast<const char*>(data), length); }

    // 获取数据
    const std::string& data() const { return m_data; }
    std::string& data() { return m_data; }

    // END_STREAM 标志
    bool is_end_stream() const { return m_header.has_flag(Http2FrameFlags::kEndStream); }
    void set_end_stream(bool value) {
        if (value) m_header.set_flag(Http2FrameFlags::kEndStream);
        else m_header.clear_flag(Http2FrameFlags::kEndStream);
    }

    // PADDED 标志
    bool is_padded() const { return m_header.has_flag(Http2FrameFlags::kPadded); }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2DataFrame clone() const {
        Http2DataFrame copy;
        copy.m_header = m_header;
        copy.m_data = m_data;
        copy.m_pad_length = m_pad_length;
        return copy;
    }

private:
    Http2DataFrame(const Http2DataFrame&) = delete;
    Http2DataFrame& operator=(const Http2DataFrame&) = delete;

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
    Http2HeadersFrame(Http2HeadersFrame&&) noexcept = default;
    Http2HeadersFrame& operator=(Http2HeadersFrame&&) noexcept = default;

    // 设置头部块片段
    void set_header_block(std::string block) { m_header_block = std::move(block); }
    const std::string& header_block() const { return m_header_block; }
    std::string& header_block() { return m_header_block; }

    // END_STREAM 标志
    bool is_end_stream() const { return m_header.has_flag(Http2FrameFlags::kEndStream); }
    void set_end_stream(bool value) {
        if (value) m_header.set_flag(Http2FrameFlags::kEndStream);
        else m_header.clear_flag(Http2FrameFlags::kEndStream);
    }

    // END_HEADERS 标志
    bool is_end_headers() const { return m_header.has_flag(Http2FrameFlags::kEndHeaders); }
    void set_end_headers(bool value) {
        if (value) m_header.set_flag(Http2FrameFlags::kEndHeaders);
        else m_header.clear_flag(Http2FrameFlags::kEndHeaders);
    }

    // PRIORITY 标志
    bool has_priority() const { return m_header.has_flag(Http2FrameFlags::kPriority); }

    // PADDED 标志
    bool is_padded() const { return m_header.has_flag(Http2FrameFlags::kPadded); }

    // 优先级字段
    bool exclusive() const { return m_exclusive; }
    uint32_t stream_dependency() const { return m_stream_dependency; }
    uint8_t weight() const { return m_weight; }

    void set_priority(bool exclusive, uint32_t stream_dependency, uint8_t weight) {
        m_header.set_flag(Http2FrameFlags::kPriority);
        m_exclusive = exclusive;
        m_stream_dependency = stream_dependency;
        m_weight = weight;
    }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2HeadersFrame clone() const {
        Http2HeadersFrame copy;
        copy.m_header = m_header;
        copy.m_stream_dependency = m_stream_dependency;
        copy.m_header_block = m_header_block;
        copy.m_exclusive = m_exclusive;
        copy.m_weight = m_weight;
        copy.m_pad_length = m_pad_length;
        return copy;
    }

private:
    Http2HeadersFrame(const Http2HeadersFrame&) = delete;
    Http2HeadersFrame& operator=(const Http2HeadersFrame&) = delete;

    uint32_t m_stream_dependency = 0;
    std::string m_header_block;
    bool m_exclusive = false;
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
    Http2PriorityFrame(Http2PriorityFrame&&) noexcept = default;
    Http2PriorityFrame& operator=(Http2PriorityFrame&&) noexcept = default;

    bool exclusive() const { return m_exclusive; }
    uint32_t stream_dependency() const { return m_stream_dependency; }
    uint8_t weight() const { return m_weight; }

    void set_priority(bool exclusive, uint32_t stream_dependency, uint8_t weight) {
        m_exclusive = exclusive;
        m_stream_dependency = stream_dependency;
        m_weight = weight;
    }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2PriorityFrame clone() const {
        Http2PriorityFrame copy;
        copy.m_header = m_header;
        copy.m_stream_dependency = m_stream_dependency;
        copy.m_exclusive = m_exclusive;
        copy.m_weight = m_weight;
        return copy;
    }

private:
    Http2PriorityFrame(const Http2PriorityFrame&) = delete;
    Http2PriorityFrame& operator=(const Http2PriorityFrame&) = delete;

    uint32_t m_stream_dependency = 0;
    bool m_exclusive = false;
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
    Http2RstStreamFrame(Http2RstStreamFrame&&) noexcept = default;
    Http2RstStreamFrame& operator=(Http2RstStreamFrame&&) noexcept = default;

    Http2ErrorCode error_code() const { return m_error_code; }
    void set_error_code(Http2ErrorCode code) { m_error_code = code; }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2RstStreamFrame clone() const {
        Http2RstStreamFrame copy;
        copy.m_header = m_header;
        copy.m_error_code = m_error_code;
        return copy;
    }

private:
    Http2RstStreamFrame(const Http2RstStreamFrame&) = delete;
    Http2RstStreamFrame& operator=(const Http2RstStreamFrame&) = delete;

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
    Http2SettingsFrame(Http2SettingsFrame&&) noexcept = default;
    Http2SettingsFrame& operator=(Http2SettingsFrame&&) noexcept = default;

    // ACK 标志
    bool is_ack() const { return m_header.has_flag(Http2FrameFlags::kAck); }
    void set_ack(bool value) {
        if (value) m_header.set_flag(Http2FrameFlags::kAck);
        else m_header.clear_flag(Http2FrameFlags::kAck);
    }

    // 设置参数
    struct Setting {
        uint32_t value;
        Http2SettingsId id;
    };

    void add_setting(Http2SettingsId id, uint32_t value) {
        m_settings.push_back(Setting{.value = value, .id = id});
    }

    const std::vector<Setting>& settings() const { return m_settings; }

    // 获取特定设置值
    std::optional<uint32_t> get_setting(Http2SettingsId id) const {
        for (const auto& s : m_settings) {
            if (s.id == id) return s.value;
        }
        return std::nullopt;
    }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2SettingsFrame clone() const {
        Http2SettingsFrame copy;
        copy.m_header = m_header;
        copy.m_settings = m_settings;
        return copy;
    }

private:
    Http2SettingsFrame(const Http2SettingsFrame&) = delete;
    Http2SettingsFrame& operator=(const Http2SettingsFrame&) = delete;

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
    Http2PushPromiseFrame(Http2PushPromiseFrame&&) noexcept = default;
    Http2PushPromiseFrame& operator=(Http2PushPromiseFrame&&) noexcept = default;

    uint32_t promised_stream_id() const { return m_promised_stream_id; }
    void set_promised_stream_id(uint32_t id) { m_promised_stream_id = id; }

    const std::string& header_block() const { return m_header_block; }
    void set_header_block(std::string block) { m_header_block = std::move(block); }

    bool is_end_headers() const { return m_header.has_flag(Http2FrameFlags::kEndHeaders); }
    void set_end_headers(bool value) {
        if (value) m_header.set_flag(Http2FrameFlags::kEndHeaders);
        else m_header.clear_flag(Http2FrameFlags::kEndHeaders);
    }

    // PADDED 标志
    bool is_padded() const { return m_header.has_flag(Http2FrameFlags::kPadded); }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2PushPromiseFrame clone() const {
        Http2PushPromiseFrame copy;
        copy.m_header = m_header;
        copy.m_promised_stream_id = m_promised_stream_id;
        copy.m_header_block = m_header_block;
        copy.m_pad_length = m_pad_length;
        return copy;
    }

private:
    Http2PushPromiseFrame(const Http2PushPromiseFrame&) = delete;
    Http2PushPromiseFrame& operator=(const Http2PushPromiseFrame&) = delete;

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
    Http2PingFrame(Http2PingFrame&&) noexcept = default;
    Http2PingFrame& operator=(Http2PingFrame&&) noexcept = default;

    // ACK 标志
    bool is_ack() const { return m_header.has_flag(Http2FrameFlags::kAck); }
    void set_ack(bool value) {
        if (value) m_header.set_flag(Http2FrameFlags::kAck);
        else m_header.clear_flag(Http2FrameFlags::kAck);
    }

    // 8 字节不透明数据
    const uint8_t* opaque_data() const { return m_opaque_data; }
    void set_opaque_data(const uint8_t* data) {
        std::memcpy(m_opaque_data, data, 8);
    }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2PingFrame clone() const {
        Http2PingFrame copy;
        copy.m_header = m_header;
        std::memcpy(copy.m_opaque_data, m_opaque_data, sizeof(m_opaque_data));
        return copy;
    }

private:
    Http2PingFrame(const Http2PingFrame&) = delete;
    Http2PingFrame& operator=(const Http2PingFrame&) = delete;

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
    Http2GoAwayFrame(Http2GoAwayFrame&&) noexcept = default;
    Http2GoAwayFrame& operator=(Http2GoAwayFrame&&) noexcept = default;

    uint32_t last_stream_id() const { return m_last_stream_id; }
    void set_last_stream_id(uint32_t id) { m_last_stream_id = id; }

    Http2ErrorCode error_code() const { return m_error_code; }
    void set_error_code(Http2ErrorCode code) { m_error_code = code; }

    const std::string& debug_data() const { return m_debug_data; }
    void set_debug_data(std::string data) { m_debug_data = std::move(data); }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2GoAwayFrame clone() const {
        Http2GoAwayFrame copy;
        copy.m_header = m_header;
        copy.m_last_stream_id = m_last_stream_id;
        copy.m_debug_data = m_debug_data;
        copy.m_error_code = m_error_code;
        return copy;
    }

private:
    Http2GoAwayFrame(const Http2GoAwayFrame&) = delete;
    Http2GoAwayFrame& operator=(const Http2GoAwayFrame&) = delete;

    uint32_t m_last_stream_id = 0;
    std::string m_debug_data;
    Http2ErrorCode m_error_code = Http2ErrorCode::NoError;
};

/**
 * @brief WINDOW_UPDATE 帧
 * @details 用于流量控制
 */
class Http2WindowUpdateFrame : public Http2Frame
{
public:
    Http2WindowUpdateFrame() { m_header.type = Http2FrameType::WindowUpdate; }
    Http2WindowUpdateFrame(Http2WindowUpdateFrame&&) noexcept = default;
    Http2WindowUpdateFrame& operator=(Http2WindowUpdateFrame&&) noexcept = default;

    uint32_t window_size_increment() const { return m_window_size_increment; }
    void set_window_size_increment(uint32_t increment) { m_window_size_increment = increment; }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2WindowUpdateFrame clone() const {
        Http2WindowUpdateFrame copy;
        copy.m_header = m_header;
        copy.m_window_size_increment = m_window_size_increment;
        return copy;
    }

private:
    Http2WindowUpdateFrame(const Http2WindowUpdateFrame&) = delete;
    Http2WindowUpdateFrame& operator=(const Http2WindowUpdateFrame&) = delete;

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
    Http2ContinuationFrame(Http2ContinuationFrame&&) noexcept = default;
    Http2ContinuationFrame& operator=(Http2ContinuationFrame&&) noexcept = default;

    const std::string& header_block() const { return m_header_block; }
    void set_header_block(std::string block) { m_header_block = std::move(block); }

    bool is_end_headers() const { return m_header.has_flag(Http2FrameFlags::kEndHeaders); }
    void set_end_headers(bool value) {
        if (value) m_header.set_flag(Http2FrameFlags::kEndHeaders);
        else m_header.clear_flag(Http2FrameFlags::kEndHeaders);
    }

    std::string serialize() const override;
    Http2ErrorCode parse_payload(const uint8_t* data, size_t length) override;
    Http2ContinuationFrame clone() const {
        Http2ContinuationFrame copy;
        copy.m_header = m_header;
        copy.m_header_block = m_header_block;
        return copy;
    }

private:
    Http2ContinuationFrame(const Http2ContinuationFrame&) = delete;
    Http2ContinuationFrame& operator=(const Http2ContinuationFrame&) = delete;

    std::string m_header_block;
};

// ==================== Http2Frame::asXXX 内联定义 ====================

inline Http2DataFrame* Http2Frame::as_data() { return is_data() ? static_cast<Http2DataFrame*>(this) : nullptr; }
inline Http2HeadersFrame* Http2Frame::as_headers() { return is_headers() ? static_cast<Http2HeadersFrame*>(this) : nullptr; }
inline Http2PriorityFrame* Http2Frame::as_priority() { return is_priority() ? static_cast<Http2PriorityFrame*>(this) : nullptr; }
inline Http2RstStreamFrame* Http2Frame::as_rst_stream() { return is_rst_stream() ? static_cast<Http2RstStreamFrame*>(this) : nullptr; }
inline Http2SettingsFrame* Http2Frame::as_settings() { return is_settings() ? static_cast<Http2SettingsFrame*>(this) : nullptr; }
inline Http2PushPromiseFrame* Http2Frame::as_push_promise() { return is_push_promise() ? static_cast<Http2PushPromiseFrame*>(this) : nullptr; }
inline Http2PingFrame* Http2Frame::as_ping() { return is_ping() ? static_cast<Http2PingFrame*>(this) : nullptr; }
inline Http2GoAwayFrame* Http2Frame::as_go_away() { return is_go_away() ? static_cast<Http2GoAwayFrame*>(this) : nullptr; }
inline Http2WindowUpdateFrame* Http2Frame::as_window_update() { return is_window_update() ? static_cast<Http2WindowUpdateFrame*>(this) : nullptr; }
inline Http2ContinuationFrame* Http2Frame::as_continuation() { return is_continuation() ? static_cast<Http2ContinuationFrame*>(this) : nullptr; }

inline const Http2DataFrame* Http2Frame::as_data() const { return is_data() ? static_cast<const Http2DataFrame*>(this) : nullptr; }
inline const Http2HeadersFrame* Http2Frame::as_headers() const { return is_headers() ? static_cast<const Http2HeadersFrame*>(this) : nullptr; }
inline const Http2PriorityFrame* Http2Frame::as_priority() const { return is_priority() ? static_cast<const Http2PriorityFrame*>(this) : nullptr; }
inline const Http2RstStreamFrame* Http2Frame::as_rst_stream() const { return is_rst_stream() ? static_cast<const Http2RstStreamFrame*>(this) : nullptr; }
inline const Http2SettingsFrame* Http2Frame::as_settings() const { return is_settings() ? static_cast<const Http2SettingsFrame*>(this) : nullptr; }
inline const Http2PushPromiseFrame* Http2Frame::as_push_promise() const { return is_push_promise() ? static_cast<const Http2PushPromiseFrame*>(this) : nullptr; }
inline const Http2PingFrame* Http2Frame::as_ping() const { return is_ping() ? static_cast<const Http2PingFrame*>(this) : nullptr; }
inline const Http2GoAwayFrame* Http2Frame::as_go_away() const { return is_go_away() ? static_cast<const Http2GoAwayFrame*>(this) : nullptr; }
inline const Http2WindowUpdateFrame* Http2Frame::as_window_update() const { return is_window_update() ? static_cast<const Http2WindowUpdateFrame*>(this) : nullptr; }
inline const Http2ContinuationFrame* Http2Frame::as_continuation() const { return is_continuation() ? static_cast<const Http2ContinuationFrame*>(this) : nullptr; }

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
    static Http2FrameHeader parse_header(const uint8_t* data);

    /**
     * @brief 解析完整帧
     * @param data 数据指针
     * @param length 数据长度
     * @return 解析结果：帧指针或错误码
     */
    static std::expected<Http2Frame::uptr, Http2ErrorCode> parse_frame(const uint8_t* data, size_t length);

    /**
     * @brief 根据帧类型创建帧对象
     * @param type 帧类型
     * @return 帧对象
     */
    static Http2Frame::uptr create_frame(Http2FrameType type);
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
