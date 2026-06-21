/**
 * @file rpc_metadata.h
 * @brief RPC调用元数据
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供每次RPC调用可携带的键值元数据容器。键名采用ASCII token风格，
 *          值按字节串保存，可承载文本或二进制内容。
 */

#ifndef GALAY_RPC_METADATA_H
#define GALAY_RPC_METADATA_H

#include "../protoc/rpc_error.h"

#include <cctype>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace galay::rpc
{

constexpr size_t kRpcMetadataMaxKeySize = 128;  ///< metadata键名最大字节数
constexpr size_t kRpcMetadataMaxValueSize = 65535;  ///< metadata值最大字节数，受wire格式uint16长度约束
constexpr size_t kRpcMetadataMaxEntries = 256;  ///< 单次RPC最多metadata条目数
constexpr size_t kRpcMetadataMaxWireSize = 64 * 1024;  ///< 单次RPC metadata wire编码最大字节数

/**
 * @brief RPC metadata键值容器
 *
 * @details RpcMetadata负责保存和校验调用级元数据。所有键名必须
 *          非空、长度不超过kRpcMetadataMaxKeySize，并且只包含ASCII字母、数字、
 *          '_'、'-'、'.'。值按字节串保存，但长度不能超过wire格式可表达范围。
 *          成员函数不做内部同步；跨线程共享时由调用方负责外部同步。
 */
class RpcMetadata {
public:
    using Storage = std::unordered_map<std::string, std::string>;
    using const_iterator = Storage::const_iterator;

    /**
     * @brief 校验metadata键名
     * @param key 待校验键名
     * @return 成功或INVALID_REQUEST错误
     */
    static std::expected<void, RpcError> validateKey(std::string_view key) {
        if (key.empty()) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "Metadata key is empty"));
        }
        if (key.size() > kRpcMetadataMaxKeySize) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "Metadata key is too long"));
        }
        for (unsigned char ch : key) {
            if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
                continue;
            }
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "Metadata key contains invalid character"));
        }
        return {};
    }

    /**
     * @brief 插入或覆盖元数据
     * @param key ASCII token风格键名
     * @param value 字节串值，会被容器持有
     * @return 成功或INVALID_REQUEST错误
     */
    std::expected<void, RpcError> insert(std::string_view key, std::string_view value) {
        auto validation = validateKey(key);
        if (!validation.has_value()) {
            return std::unexpected(validation.error());
        }
        if (value.size() > kRpcMetadataMaxValueSize) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "Metadata value is too large"));
        }
        std::string key_string(key);
        if (!m_values.contains(key_string) && m_values.size() >= kRpcMetadataMaxEntries) {
            return std::unexpected(RpcError(RpcErrorCode::RESOURCE_EXHAUSTED,
                                            "Metadata entry limit exceeded"));
        }
        const size_t previous_size = encodedValueSize();
        size_t next_size = previous_size;
        if (auto it = m_values.find(key_string); it != m_values.end()) {
            next_size -= encodedEntrySize(it->first, it->second);
        }
        next_size += encodedEntrySize(key, value);
        if (next_size > kRpcMetadataMaxWireSize) {
            return std::unexpected(RpcError(RpcErrorCode::RESOURCE_EXHAUSTED,
                                            "Metadata wire size limit exceeded"));
        }
        m_values.insert_or_assign(std::move(key_string), std::string(value));
        return {};
    }

    /**
     * @brief 查找元数据值
     * @param key 键名
     * @return 存在时返回只读字符串视图；不存在或键名非法时返回空
     * @note 返回视图借用容器内部存储，修改容器后可能失效。
     */
    std::optional<std::string_view> get(std::string_view key) const {
        if (!validateKey(key).has_value()) {
            return std::nullopt;
        }
        auto it = m_values.find(std::string(key));
        if (it == m_values.end()) {
            return std::nullopt;
        }
        return std::string_view(it->second);
    }

    /**
     * @brief 移除元数据
     * @param key 键名
     * @return 是否移除了已存在的键
     */
    bool remove(std::string_view key) {
        if (!validateKey(key).has_value()) {
            return false;
        }
        return m_values.erase(std::string(key)) != 0;
    }

    /// @brief 清空所有元数据
    void clear() { m_values.clear(); }
    /// @brief 当前键值数量
    size_t size() const { return m_values.size(); }
    /// @brief 是否为空
    bool empty() const { return m_values.empty(); }
    /// @brief 只读迭代起点
    const_iterator begin() const { return m_values.begin(); }
    /// @brief 只读迭代终点
    const_iterator end() const { return m_values.end(); }

private:
    static size_t encodedEntrySize(std::string_view key, std::string_view value) {
        return sizeof(uint16_t) + sizeof(uint16_t) + key.size() + value.size();
    }

    size_t encodedValueSize() const {
        if (m_values.empty()) {
            return 0;
        }
        size_t size = sizeof(uint16_t) + sizeof(uint16_t);
        for (const auto& [key, value] : m_values) {
            size += encodedEntrySize(key, value);
        }
        return size;
    }

    Storage m_values;  ///< 持有的元数据键值
};

}  // namespace galay::rpc

#endif  // GALAY_RPC_METADATA_H
