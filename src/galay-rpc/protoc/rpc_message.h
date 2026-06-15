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
#include <cstring>
#include <vector>
#include <expected>

namespace galay::rpc
{

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

    /// @brief 获取payload总字节数
    size_t size() const { return segment1_len + segment2_len; }
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
        m_type     = buffer[5];
        m_flags    = buffer[6];
        m_reserved = buffer[7];

        uint32_t request_id, body_length;
        std::memcpy(&request_id, buffer + 8, 4);
        std::memcpy(&body_length, buffer + 12, 4);

        m_request_id  = rpcNtohl(request_id);
        m_body_length = rpcNtohl(body_length);

        return m_body_length <= RPC_MAX_BODY_SIZE;
    }
};

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

    /**
     * @brief 序列化请求
     */
    std::vector<char> serialize() const {
        RpcPayloadView payload_view = payloadView();
        size_t body_size = 2 + m_service_name.size() + 2 + m_method_name.size() + payload_view.size();
        std::vector<char> buffer(RPC_HEADER_SIZE + body_size);

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::REQUEST);
        header.m_flags = rpcEncodeFlags(m_call_mode, m_end_of_stream);
        header.m_request_id = m_request_id;
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(buffer.data());

        char* body = buffer.data() + RPC_HEADER_SIZE;
        size_t offset = 0;

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
        if (length < 4) return false;

        size_t offset = 0;

        // service name
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

    /**
     * @brief 构造响应
     * @param request_id 对应的请求ID
     * @param error_code 错误码，默认OK
     */
    RpcResponse(uint32_t request_id, RpcErrorCode error_code = RpcErrorCode::OK)
        : m_request_id(request_id)
        , m_error_code(error_code) {}

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
     * @brief 序列化响应
     */
    std::vector<char> serialize() const {
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
