/**
 * @file trace_id.h
 * @brief 追踪标识符（TraceId）定义
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义 16 字节（128 位）的追踪标识符，提供十六进制解析、格式化、
 * 随机生成及有效性校验等操作。TraceId 在分布式追踪中唯一标识一条完整追踪链路。
 */

#pragma once

#include "galay-tracing/common/id_format.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace galay::tracing {

/**
 * @brief 追踪标识符
 * @details 16 字节（128 位）标识符，用于在分布式追踪中唯一标识一条追踪链路。
 * 支持十六进制字符串的解析和格式化，以及随机生成。全零值视为无效。
 */
class TraceId {
public:
    static constexpr std::size_t kByteLength = 16;  ///< TraceId 字节长度
    static constexpr std::size_t kHexLength = kByteLength * 2; ///< TraceId 十六进制字符串长度

    using Bytes = std::array<std::byte, kByteLength>; ///< TraceId 底层字节数组类型

    constexpr TraceId() noexcept = default; ///< 默认构造，初始化为全零（无效）

    /**
     * @brief 获取全零（无效）的 TraceId
     * @return 全零的 TraceId 实例
     */
    [[nodiscard]] static constexpr TraceId zero() noexcept {
        return TraceId{};
    }

    /**
     * @brief 随机生成一个有效的 TraceId
     * @return 随机生成的 TraceId
     */
    [[nodiscard]] static TraceId random() noexcept;

    /**
     * @brief 从十六进制字符串解析 TraceId
     * @details 需精确 32 个十六进制字符，无效、格式错误或全零输入返回 zero()
     * @param hex 十六进制字符串视图
     * @return 解析成功返回对应 TraceId，否则返回 zero()
     */
    [[nodiscard]] static TraceId fromHex(std::string_view hex) noexcept {
        TraceId id;
        if (!detail::parseHex(hex, id.m_bytes.data(), id.m_bytes.size()) || !id.isValid()) {
            return zero();
        }
        return id;
    }

    /**
     * @brief 检查 TraceId 是否有效（非全零）
     * @return 有效返回 true，全零返回 false
     */
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return detail::hasNonZeroByte(m_bytes.data(), m_bytes.size());
    }

    /**
     * @brief 将 TraceId 转换为十六进制字符串
     * @return 32 个字符的小写十六进制字符串
     */
    [[nodiscard]] std::string toHex() const {
        std::string hex(kHexLength, '\0');
        static_cast<void>(toHex(hex.data(), hex.size()));
        return hex;
    }

    /**
     * @brief 将 TraceId 格式化为十六进制字符串写入给定缓冲区
     * @param out 输出缓冲区指针
     * @param len 输出缓冲区大小（至少 kHexLength）
     * @return 成功返回 true，缓冲区不足返回 false
     */
    [[nodiscard]] bool toHex(char* out, std::size_t len) const noexcept {
        return detail::formatHex(m_bytes.data(), m_bytes.size(), out, len);
    }

    /**
     * @brief 将 TraceId 格式化为固定大小的十六进制字符数组
     * @return 包含 kHexLength 个字符的数组
     */
    [[nodiscard]] std::array<char, kHexLength> toHexArray() const noexcept {
        std::array<char, kHexLength> hex{};
        static_cast<void>(toHex(hex.data(), hex.size()));
        return hex;
    }

    /**
     * @brief 获取底层字节数组的只读引用
     * @return 16 字节数组的常量引用
     */
    [[nodiscard]] constexpr const Bytes& bytes() const noexcept {
        return m_bytes;
    }

    friend constexpr bool operator==(const TraceId&, const TraceId&) noexcept = default;

private:
    friend TraceId detail::makeRandomTraceId() noexcept;

    explicit constexpr TraceId(Bytes bytes) noexcept
        : m_bytes(bytes) {
    }

    Bytes m_bytes{}; ///< 底层字节数据
};

} // namespace galay::tracing
