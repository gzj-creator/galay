/**
 * @file rpc_message.h
 * @brief RPC消息定义
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details RPC消息协议格式：
 * +--------+--------+--------+--------+--------+--------+--------+--------+
 * |              Magic (4 bytes)              |  Ver   |  Type  | Flags  |
 * +--------+--------+--------+--------+--------+--------+--------+--------+
 * |           Request ID (4 bytes)            |       Body Length (4 bytes)
 * +--------+--------+--------+--------+--------+--------+--------+--------+
 * |                           Body (variable)                             |
 * +--------+--------+--------+--------+--------+--------+--------+--------+
 *
 * Body格式 (Request):
 * - optional metadata extension when header reserved bit marks it present:
 *   metadata marker (2 bytes, 0xFFFF) + metadata_count + key/value pairs
 * - service_name_len (2 bytes) + service_name
 * - method_name_len (2 bytes) + method_name
 * - payload
 *
 * Body格式 (Response):
 * - error_code (2 bytes)
 * - payload
 */

#ifndef GALAY_RPC_MESSAGE_H
#define GALAY_RPC_MESSAGE_H

#include "rpc_base.h"
#include "rpc_error.h"
#include "../kernel/rpc_metadata.h"
#include <cstring>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::rpc
{

inline constexpr uint16_t kRpcRequestMetadataMarker = 0xFFFF;  ///< 请求metadata扩展标记

inline bool rpcHeartbeatBodyIsValid(uint8_t type, uint32_t body_length) {
    return type != static_cast<uint8_t>(RpcMessageType::HEARTBEAT) || body_length == 0;
}

/**
 * @brief RPC payload 零拷贝视图（最多两段，适配环形缓冲区）
 *
 * @note 本视图仅借用外部内存，不拥有数据。
 *       调用方必须保证视图覆盖的内存在使用期间保持有效。
 */
struct RpcPayloadView {
    const char* segment1 = nullptr;   ///< 第一段数据指针
    size_t segment1_len = 0;          ///< 第一段数据长度
    const char* segment2 = nullptr;   ///< 第二段数据指针
    size_t segment2_len = 0;          ///< 第二段数据长度

    /// @brief 获取payload总字节数；溢出时饱和到size_t最大值以便上层边界校验拒绝
    size_t size() const {
        if (segment1_len > std::numeric_limits<size_t>::max() - segment2_len) {
            return std::numeric_limits<size_t>::max();
        }
        return segment1_len + segment2_len;
    }
    /// @brief 判断payload是否为空
    bool empty() const { return size() == 0; }
};

/**
 * @brief RPC消息头
 */
struct RpcHeader {
    uint32_t m_magic = RPC_MAGIC;       ///< 魔数
    uint8_t m_version = RPC_VERSION;    ///< 版本
    uint8_t m_type = 0;                 ///< 消息类型
    uint8_t m_flags = 0;                ///< 标志位
    uint8_t m_reserved = 0;             ///< 保留
    uint32_t m_request_id = 0;          ///< 请求ID
    uint32_t m_body_length = 0;         ///< 消息体长度

    /**
     * @brief 序列化到缓冲区
     */
    void serialize(char* buffer) const {
        uint32_t magic = rpcHtonl(m_magic);
        uint32_t request_id = rpcHtonl(m_request_id);
        uint32_t body_length = rpcHtonl(m_body_length);

        std::memcpy(buffer, &magic, 4);
        buffer[4] = m_version;
        buffer[5] = m_type;
        buffer[6] = m_flags;
        buffer[7] = m_reserved;
        std::memcpy(buffer + 8, &request_id, 4);
        std::memcpy(buffer + 12, &body_length, 4);
    }

    /**
     * @brief 从缓冲区反序列化
     */
    bool deserialize(const char* buffer) {
        uint32_t magic;
        std::memcpy(&magic, buffer, 4);
        m_magic = rpcNtohl(magic);

        if (m_magic != RPC_MAGIC) {
            return false;
        }

        m_version  = buffer[4];
        if (m_version != RPC_VERSION) {
            return false;
        }

        m_type     = buffer[5];
        m_flags    = buffer[6];
        m_reserved = buffer[7];

        uint32_t request_id, body_length;
        std::memcpy(&request_id, buffer + 8, 4);
        std::memcpy(&body_length, buffer + 12, 4);

        m_request_id  = rpcNtohl(request_id);
        m_body_length = rpcNtohl(body_length);

        return m_body_length <= RPC_MAX_BODY_SIZE &&
               rpcHeartbeatBodyIsValid(m_type, m_body_length);
    }
};

/**
 * @brief 构造HEARTBEAT帧
 * @param request_id 心跳ID，pong复用相同ID
 * @return 完整HEARTBEAT消息字节
 */
inline std::vector<char> rpcBuildHeartbeatFrame(uint32_t request_id) {
    std::vector<char> frame(RPC_HEADER_SIZE);
    RpcHeader header;
    header.m_type = static_cast<uint8_t>(RpcMessageType::HEARTBEAT);
    header.m_flags = rpcEncodeFlags(RpcCallMode::UNARY, true);
    header.m_request_id = request_id;
    header.m_body_length = 0;
    header.serialize(frame.data());
    return frame;
}

/**
 * @brief RPC请求消息
 */
/**
 * @brief RPC请求消息
 *
 * @details 包含请求ID、调用模式、服务名、方法名和payload，
 *          支持自有缓冲和零拷贝借用两种payload模式。
 */
class RpcRequest {
public:
    RpcRequest() = default;

private:
    RpcRequest(const RpcRequest&) = delete;
    RpcRequest& operator=(const RpcRequest&) = delete;
public:

    RpcRequest(RpcRequest&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    RpcRequest& operator=(RpcRequest&& other) noexcept
    {
        if (this != &other) {
            moveFrom(std::move(other));
        }
        return *this;
    }

    /**
     * @brief 构造请求
     * @param request_id 请求ID
     * @param service 服务名
     * @param method 方法名
     */
    RpcRequest(uint32_t request_id, std::string_view service, std::string_view method)
        : m_request_id(request_id)
        , m_service_name(service)
        , m_method_name(method) {}

    /**
     * @brief 显式深拷贝请求
     * @return 持有独立payload缓冲的新请求
     *
     * @note 借用payload会被复制为自有缓冲，避免clone结果继续依赖外部内存。
     */
    RpcRequest clone() const {
        RpcRequest copy;
        copy.m_request_id = m_request_id;
        copy.m_call_mode = m_call_mode;
        copy.m_end_of_stream = m_end_of_stream;
        copy.m_service_name = m_service_name;
        copy.m_method_name = m_method_name;
        copy.m_metadata = m_metadata;
        copy.copyPayloadFromView(payloadView());
        return copy;
    }

    /// @brief 获取请求ID
    uint32_t requestId() const { return m_request_id; }
    /// @brief 设置请求ID
    void requestId(uint32_t id) { m_request_id = id; }
    /// @brief 获取调用模式
    RpcCallMode callMode() const { return m_call_mode; }
    /// @brief 设置调用模式
    void callMode(RpcCallMode mode) { m_call_mode = mode; }
    /// @brief 判断是否为流结束帧
    bool endOfStream() const { return m_end_of_stream; }
    /// @brief 设置流结束标志
    void endOfStream(bool end) { m_end_of_stream = end; }

    /// @brief 获取服务名
    const std::string& serviceName() const { return m_service_name; }
    /// @brief 设置服务名
    void serviceName(std::string_view name) { m_service_name = name; }
    /// @brief 设置服务名（移动语义）
    void serviceName(std::string&& name) { m_service_name = std::move(name); }

    /// @brief 获取方法名
    const std::string& methodName() const { return m_method_name; }
    /// @brief 设置方法名
    void methodName(std::string_view name) { m_method_name = name; }
    /// @brief 设置方法名（移动语义）
    void methodName(std::string&& name) { m_method_name = std::move(name); }

    /// @brief 获取payload数据（触发实体化拷贝）
    const std::vector<char>& payload() const {
        materializePayloadIfNeeded();
        return m_payload;
    }
    /// @brief 获取payload大小
    size_t payloadSize() const {
        return m_payload_owned ? m_payload.size() : m_payload_view.size();
    }
    /// @brief 获取payload视图（不触发拷贝）
    RpcPayloadView payloadView() const {
        if (m_payload_owned) {
            return RpcPayloadView{
                m_payload.data(),
                m_payload.size(),
                nullptr,
                0
            };
        }
        return m_payload_view;
    }
    /**
     * @brief 设置payload（拷贝模式）
     * @param data 数据指针
     * @param len 数据长度
     */
    void payload(const char* data, size_t len) {
        m_payload.assign(data, data + len);
        m_payload_owned = true;
        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
    }
    /**
     * @brief 设置payload（移动模式）
     * @param data 数据向量
     */
    void payload(std::vector<char>&& data) {
        m_payload = std::move(data);
        m_payload_owned = true;
        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
    }
    /**
     * @brief 设置payload视图（零拷贝借用模式）
     * @param view 外部payload视图
     * @note 调用方必须保证视图指向的内存在消息被消费完成前保持有效
     */
    void payloadView(const RpcPayloadView& view) {
        // 切换为借用模式：不拷贝数据，仅记录外部payload视图。
        m_payload.clear();
        m_payload_view = view;
        m_payload_owned = false;
    }

    /// @brief 获取可变metadata
    RpcMetadata& metadata() { return m_metadata; }
    /// @brief 获取只读metadata
    const RpcMetadata& metadata() const { return m_metadata; }

    /// @brief 请求体序列化后的字节数
    size_t serializedBodySize() const {
        return metadataWireSize() +
               sizeof(uint16_t) + m_service_name.size() +
               sizeof(uint16_t) + m_method_name.size() +
               payloadSize();
    }
    /// @brief metadata wire编码字节数
    size_t serializedMetadataSize() const { return metadataWireSize(); }

    /**
     * @brief 校验请求是否可安全写入协议帧
     * @return 成功或INVALID_REQUEST
     *
     * @note 写入侧必须先校验16位长度字段和body上限，避免截断后生成歧义帧。
     */
    std::expected<void, RpcError> validateForWrite() const {
        if (m_service_name.size() > std::numeric_limits<uint16_t>::max()) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC service name too large"));
        }
        if (m_method_name.size() > std::numeric_limits<uint16_t>::max()) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC method name too large"));
        }
        if (serializedMetadataSize() > kRpcMetadataMaxWireSize) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC metadata too large"));
        }
        if (payloadSize() > RPC_MAX_BODY_SIZE || serializedBodySize() > RPC_MAX_BODY_SIZE) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC request body too large"));
        }
        return {};
    }

    /**
     * @brief 序列化请求
     */
    std::vector<char> serialize() const {
        if (!validateForWrite().has_value()) {
            return {};
        }

        RpcPayloadView payload_view = payloadView();
        size_t body_size = serializedBodySize();
        std::vector<char> buffer(RPC_HEADER_SIZE + body_size);

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::REQUEST);
        header.m_flags = rpcEncodeFlags(m_call_mode, m_end_of_stream);
        if (!m_metadata.empty()) {
            header.m_reserved = RPC_RESERVED_METADATA;
        }
        header.m_request_id = m_request_id;
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(buffer.data());

        char* body = buffer.data() + RPC_HEADER_SIZE;
        size_t offset = 0;

        offset += serializeMetadata(body + offset);

        // service name
        uint16_t service_len = rpcHtons(static_cast<uint16_t>(m_service_name.size()));
        std::memcpy(body + offset, &service_len, 2);
        offset += 2;
        std::memcpy(body + offset, m_service_name.data(), m_service_name.size());
        offset += m_service_name.size();

        // method name
        uint16_t method_len = rpcHtons(static_cast<uint16_t>(m_method_name.size()));
        std::memcpy(body + offset, &method_len, 2);
        offset += 2;
        std::memcpy(body + offset, m_method_name.data(), m_method_name.size());
        offset += m_method_name.size();

        // payload
        if (payload_view.segment1_len > 0) {
            std::memcpy(body + offset, payload_view.segment1, payload_view.segment1_len);
            offset += payload_view.segment1_len;
        }
        if (payload_view.segment2_len > 0) {
            std::memcpy(body + offset, payload_view.segment2, payload_view.segment2_len);
        }

        return buffer;
    }

    /**
     * @brief 反序列化请求体
     */
    bool deserializeBody(const char* body, size_t length) {
        return deserializeBody(body, length, false);
    }

    /**
     * @brief 反序列化请求体
     * @param has_metadata header reserved位是否声明了metadata扩展
     */
    bool deserializeBody(const char* body, size_t length, bool has_metadata) {
        if (length < 4) return false;

        size_t offset = 0;

        m_metadata.clear();
        if (has_metadata) {
            uint16_t possible_marker;
            std::memcpy(&possible_marker, body, sizeof(possible_marker));
            possible_marker = rpcNtohs(possible_marker);
            if (possible_marker != kRpcRequestMetadataMarker) return false;
            offset += sizeof(uint16_t);
            uint16_t metadata_count;
            std::memcpy(&metadata_count, body + offset, sizeof(metadata_count));
            metadata_count = rpcNtohs(metadata_count);
            offset += sizeof(uint16_t);
            if (metadata_count > kRpcMetadataMaxEntries) return false;

            for (uint16_t i = 0; i < metadata_count; ++i) {
                if (offset + sizeof(uint16_t) + sizeof(uint16_t) > length) return false;
                uint16_t key_len;
                uint16_t value_len;
                std::memcpy(&key_len, body + offset, sizeof(key_len));
                offset += sizeof(uint16_t);
                std::memcpy(&value_len, body + offset, sizeof(value_len));
                offset += sizeof(uint16_t);
                key_len = rpcNtohs(key_len);
                value_len = rpcNtohs(value_len);
                if (offset + key_len + value_len > length) return false;
                std::string_view key(body + offset, key_len);
                offset += key_len;
                std::string_view value(body + offset, value_len);
                offset += value_len;
                if (!m_metadata.insert(key, value).has_value()) return false;
            }
        }

        // service name
        if (offset + 2 > length) return false;
        uint16_t service_len;
        std::memcpy(&service_len, body + offset, 2);
        service_len = rpcNtohs(service_len);
        offset += 2;

        if (offset + service_len > length) return false;
        m_service_name.assign(body + offset, service_len);
        offset += service_len;

        // method name
        if (offset + 2 > length) return false;
        uint16_t method_len;
        std::memcpy(&method_len, body + offset, 2);
        method_len = rpcNtohs(method_len);
        offset += 2;

        if (offset + method_len > length) return false;
        m_method_name.assign(body + offset, method_len);
        offset += method_len;

        // payload
        if (offset < length) {
            m_payload.assign(body + offset, body + length);
            m_payload_owned = true;
            m_payload_view = RpcPayloadView{
                m_payload.data(),
                m_payload.size(),
                nullptr,
                0
            };
        } else {
            m_payload.clear();
            m_payload_owned = true;
            m_payload_view = RpcPayloadView{};
        }

        return true;
    }

private:
    void moveFrom(RpcRequest&& other) noexcept {
        m_request_id = other.m_request_id;
        m_call_mode = other.m_call_mode;
        m_end_of_stream = other.m_end_of_stream;
        m_service_name = std::move(other.m_service_name);
        m_method_name = std::move(other.m_method_name);
        m_metadata = std::move(other.m_metadata);
        m_payload = std::move(other.m_payload);
        m_payload_view = other.m_payload_view;
        m_payload_owned = other.m_payload_owned;
        updateOwnedPayloadView();
        other.resetMovedPayload();
    }

    void updateOwnedPayloadView() const {
        if (!m_payload_owned) {
            return;
        }
        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
    }

    void resetMovedPayload() noexcept {
        m_payload.clear();
        m_payload_view = RpcPayloadView{};
        m_payload_owned = true;
    }

    void copyPayloadFromView(const RpcPayloadView& view) {
        const size_t total = view.size();
        m_payload.resize(total);
        size_t offset = 0;
        if (view.segment1_len > 0) {
            std::memcpy(m_payload.data(), view.segment1, view.segment1_len);
            offset += view.segment1_len;
        }
        if (view.segment2_len > 0) {
            std::memcpy(m_payload.data() + offset, view.segment2, view.segment2_len);
        }
        m_payload_owned = true;
        updateOwnedPayloadView();
    }

    size_t metadataWireSize() const {
        if (m_metadata.empty()) {
            return 0;
        }

        size_t size = sizeof(uint16_t) + sizeof(uint16_t);
        for (const auto& [key, value] : m_metadata) {
            size += sizeof(uint16_t) + sizeof(uint16_t) + key.size() + value.size();
        }
        if (size > kRpcMetadataMaxWireSize) {
            return kRpcMetadataMaxWireSize + 1;
        }
        return size;
    }

    size_t serializeMetadata(char* body) const {
        if (m_metadata.empty()) {
            return 0;
        }

        size_t offset = 0;
        uint16_t marker = rpcHtons(kRpcRequestMetadataMarker);
        uint16_t count = rpcHtons(static_cast<uint16_t>(m_metadata.size()));
        std::memcpy(body + offset, &marker, sizeof(marker));
        offset += sizeof(marker);
        std::memcpy(body + offset, &count, sizeof(count));
        offset += sizeof(count);
        for (const auto& [key, value] : m_metadata) {
            uint16_t key_len = rpcHtons(static_cast<uint16_t>(key.size()));
            uint16_t value_len = rpcHtons(static_cast<uint16_t>(value.size()));
            std::memcpy(body + offset, &key_len, sizeof(key_len));
            offset += sizeof(key_len);
            std::memcpy(body + offset, &value_len, sizeof(value_len));
            offset += sizeof(value_len);
            std::memcpy(body + offset, key.data(), key.size());
            offset += key.size();
            std::memcpy(body + offset, value.data(), value.size());
            offset += value.size();
        }
        return offset;
    }

    void materializePayloadIfNeeded() const {
        if (m_payload_owned) {
            return;
        }

        const size_t total = m_payload_view.size();
        m_payload.resize(total);
        size_t offset = 0;
        if (m_payload_view.segment1_len > 0) {
            std::memcpy(m_payload.data(), m_payload_view.segment1, m_payload_view.segment1_len);
            offset += m_payload_view.segment1_len;
        }
        if (m_payload_view.segment2_len > 0) {
            std::memcpy(m_payload.data() + offset, m_payload_view.segment2, m_payload_view.segment2_len);
        }

        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
        m_payload_owned = true;
    }

private:
    uint32_t m_request_id = 0;              ///< 请求ID
    RpcCallMode m_call_mode = RpcCallMode::UNARY;  ///< 调用模式
    bool m_end_of_stream = true;             ///< 流结束标志
    std::string m_service_name;              ///< 服务名
    std::string m_method_name;               ///< 方法名
    RpcMetadata m_metadata;                  ///< 请求metadata
    mutable std::vector<char> m_payload;     ///< payload缓冲区
    mutable RpcPayloadView m_payload_view{}; ///< payload零拷贝视图
    mutable bool m_payload_owned = true;     ///< 是否拥有payload数据
};

/**
 * @brief RPC响应消息
 *
 * @details 包含请求ID、调用模式、错误码和payload，
 *          支持自有缓冲和零拷贝借用两种payload模式。
 */
class RpcResponse {
public:
    RpcResponse() = default;

private:
    RpcResponse(const RpcResponse&) = delete;
    RpcResponse& operator=(const RpcResponse&) = delete;
public:

    RpcResponse(RpcResponse&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    RpcResponse& operator=(RpcResponse&& other) noexcept
    {
        if (this != &other) {
            moveFrom(std::move(other));
        }
        return *this;
    }

    /**
     * @brief 构造响应
     * @param request_id 对应的请求ID
     * @param error_code 错误码，默认OK
     */
    RpcResponse(uint32_t request_id, RpcErrorCode error_code = RpcErrorCode::OK)
        : m_request_id(request_id)
        , m_error_code(error_code) {}

    /**
     * @brief 显式深拷贝响应
     * @return 持有独立payload缓冲的新响应
     *
     * @note 借用payload会被复制为自有缓冲，避免clone结果继续依赖外部内存。
     */
    RpcResponse clone() const {
        RpcResponse copy;
        copy.m_request_id = m_request_id;
        copy.m_call_mode = m_call_mode;
        copy.m_end_of_stream = m_end_of_stream;
        copy.m_error_code = m_error_code;
        copy.copyPayloadFromView(payloadView());
        return copy;
    }

    /// @brief 获取请求ID
    uint32_t requestId() const { return m_request_id; }
    /// @brief 设置请求ID
    void requestId(uint32_t id) { m_request_id = id; }
    /// @brief 获取调用模式
    RpcCallMode callMode() const { return m_call_mode; }
    /// @brief 设置调用模式
    void callMode(RpcCallMode mode) { m_call_mode = mode; }
    /// @brief 判断是否为流结束帧
    bool endOfStream() const { return m_end_of_stream; }
    /// @brief 设置流结束标志
    void endOfStream(bool end) { m_end_of_stream = end; }

    /// @brief 获取错误码
    RpcErrorCode errorCode() const { return m_error_code; }
    /// @brief 设置错误码
    void errorCode(RpcErrorCode code) { m_error_code = code; }

    /// @brief 获取payload数据（触发实体化拷贝）
    const std::vector<char>& payload() const {
        materializePayloadIfNeeded();
        return m_payload;
    }
    /// @brief 获取payload大小
    size_t payloadSize() const {
        return m_payload_owned ? m_payload.size() : m_payload_view.size();
    }
    /// @brief 获取payload视图（不触发拷贝）
    RpcPayloadView payloadView() const {
        if (m_payload_owned) {
            return RpcPayloadView{
                m_payload.data(),
                m_payload.size(),
                nullptr,
                0
            };
        }
        return m_payload_view;
    }
    /// @brief 将借用payload实体化为自有缓冲，避免外部RingBuffer复用后悬空
    void materializePayload() const {
        materializePayloadIfNeeded();
    }
    /**
     * @brief 设置payload（拷贝模式）
     * @param data 数据指针
     * @param len 数据长度
     */
    void payload(const char* data, size_t len) {
        m_payload.assign(data, data + len);
        m_payload_owned = true;
        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
    }
    /**
     * @brief 设置payload（移动模式）
     * @param data 数据向量
     */
    void payload(std::vector<char>&& data) {
        m_payload = std::move(data);
        m_payload_owned = true;
        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
    }
    /**
     * @brief 设置payload视图（零拷贝借用模式）
     * @param view 外部payload视图
     * @note 调用方必须保证视图指向的内存在消息被消费完成前保持有效
     */
    void payloadView(const RpcPayloadView& view) {
        // 切换为借用模式：不拷贝数据，仅记录外部payload视图。
        m_payload.clear();
        m_payload_view = view;
        m_payload_owned = false;
    }

    /// @brief 判断响应是否为成功状态
    bool isOk() const { return m_error_code == RpcErrorCode::OK; }

    /**
     * @brief 校验响应是否可安全写入协议帧
     * @return 成功或INVALID_RESPONSE
     *
     * @note 响应body包含2字节错误码，写入侧必须把该字段计入上限。
     */
    std::expected<void, RpcError> validateForWrite() const {
        const auto payload_size = payloadSize();
        if (payload_size > RPC_MAX_BODY_SIZE ||
            payload_size > RPC_MAX_BODY_SIZE - sizeof(uint16_t)) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE,
                                            "RPC response body too large"));
        }
        return {};
    }

    /**
     * @brief 序列化响应
     */
    std::vector<char> serialize() const {
        if (!validateForWrite().has_value()) {
            return {};
        }

        RpcPayloadView payload_view = payloadView();
        size_t body_size = 2 + payload_view.size();
        std::vector<char> buffer(RPC_HEADER_SIZE + body_size);

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::RESPONSE);
        header.m_flags = rpcEncodeFlags(m_call_mode, m_end_of_stream);
        header.m_request_id = m_request_id;
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(buffer.data());

        char* body = buffer.data() + RPC_HEADER_SIZE;

        // error code
        uint16_t error_code = rpcHtons(static_cast<uint16_t>(m_error_code));
        std::memcpy(body, &error_code, 2);

        // payload
        size_t payload_offset = 2;
        if (payload_view.segment1_len > 0) {
            std::memcpy(body + payload_offset, payload_view.segment1, payload_view.segment1_len);
            payload_offset += payload_view.segment1_len;
        }
        if (payload_view.segment2_len > 0) {
            std::memcpy(body + payload_offset, payload_view.segment2, payload_view.segment2_len);
        }

        return buffer;
    }

    /**
     * @brief 反序列化响应体
     */
    bool deserializeBody(const char* body, size_t length) {
        if (length < 2) return false;

        uint16_t error_code;
        std::memcpy(&error_code, body, 2);
        m_error_code = static_cast<RpcErrorCode>(rpcNtohs(error_code));

        if (length > 2) {
            m_payload.assign(body + 2, body + length);
            m_payload_owned = true;
            m_payload_view = RpcPayloadView{
                m_payload.data(),
                m_payload.size(),
                nullptr,
                0
            };
        } else {
            m_payload.clear();
            m_payload_owned = true;
            m_payload_view = RpcPayloadView{};
        }

        return true;
    }

private:
    void moveFrom(RpcResponse&& other) noexcept {
        m_request_id = other.m_request_id;
        m_call_mode = other.m_call_mode;
        m_end_of_stream = other.m_end_of_stream;
        m_error_code = other.m_error_code;
        m_payload = std::move(other.m_payload);
        m_payload_view = other.m_payload_view;
        m_payload_owned = other.m_payload_owned;
        updateOwnedPayloadView();
        other.resetMovedPayload();
    }

    void updateOwnedPayloadView() const {
        if (!m_payload_owned) {
            return;
        }
        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
    }

    void resetMovedPayload() noexcept {
        m_payload.clear();
        m_payload_view = RpcPayloadView{};
        m_payload_owned = true;
    }

    void copyPayloadFromView(const RpcPayloadView& view) {
        const size_t total = view.size();
        m_payload.resize(total);
        size_t offset = 0;
        if (view.segment1_len > 0) {
            std::memcpy(m_payload.data(), view.segment1, view.segment1_len);
            offset += view.segment1_len;
        }
        if (view.segment2_len > 0) {
            std::memcpy(m_payload.data() + offset, view.segment2, view.segment2_len);
        }
        m_payload_owned = true;
        updateOwnedPayloadView();
    }

    void materializePayloadIfNeeded() const {
        if (m_payload_owned) {
            return;
        }

        const size_t total = m_payload_view.size();
        m_payload.resize(total);
        size_t offset = 0;
        if (m_payload_view.segment1_len > 0) {
            std::memcpy(m_payload.data(), m_payload_view.segment1, m_payload_view.segment1_len);
            offset += m_payload_view.segment1_len;
        }
        if (m_payload_view.segment2_len > 0) {
            std::memcpy(m_payload.data() + offset, m_payload_view.segment2, m_payload_view.segment2_len);
        }

        m_payload_view = RpcPayloadView{
            m_payload.data(),
            m_payload.size(),
            nullptr,
            0
        };
        m_payload_owned = true;
    }

private:
    uint32_t m_request_id = 0;              ///< 请求ID
    RpcCallMode m_call_mode = RpcCallMode::UNARY;  ///< 调用模式
    bool m_end_of_stream = true;             ///< 流结束标志
    RpcErrorCode m_error_code = RpcErrorCode::OK;  ///< 错误码
    mutable std::vector<char> m_payload;     ///< payload缓冲区
    mutable RpcPayloadView m_payload_view{}; ///< payload零拷贝视图
    mutable bool m_payload_owned = true;     ///< 是否拥有payload数据
};

} // namespace galay::rpc

#endif // GALAY_RPC_MESSAGE_H
