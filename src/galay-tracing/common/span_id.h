/**
 * @file span_id.h
 * @brief Span 标识符（SpanId）定义
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义 8 字节（64 位）的 Span 标识符，提供十六进制解析、格式化、
 * 随机生成及有效性校验等操作。SpanId 在分布式追踪中唯一标识一个 Span。
 */

#pragma once

#include "galay-tracing/common/id_format.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace galay::tracing {

/**
 * @brief Span 标识符
 * @details 8 字节（64 位）标识符，用于在追踪中唯一标识一个 Span。
 * 支持十六进制字符串的解析和格式化，以及随机生成。全零值视为无效。
 */
class SpanId {
public:
    static constexpr std::size_t kByteLength = 8;   ///< SpanId 字节长度
    static constexpr std::size_t kHexLength = kByteLength * 2; ///< SpanId 十六进制字符串长度

    using Bytes = std::array<std::byte, kByteLength>; ///< SpanId 底层字节数组类型

    constexpr SpanId() noexcept = default; ///< 默认构造，初始化为全零（无效）

    /**
     * @brief 获取全零（无效）的 SpanId
     * @return 全零的 SpanId 实例
     */
    [[nodiscard]] static constexpr SpanId zero() noexcept {
        return SpanId{};
    }

    /**
     * @brief 随机生成一个有效的 SpanId
     * @return 随机生成的 SpanId
     */
    [[nodiscard]] static SpanId random() noexcept;

    /**
     * @brief 从十六进制字符串解析 SpanId
     * @details 需精确 16 个十六进制字符，无效、格式错误或全零输入返回 zero()
     * @param hex 十六进制字符串视图
     * @return 解析成功返回对应 SpanId，否则返回 zero()
     */
    [[nodiscard]] static SpanId fromHex(std::string_view hex) noexcept {
        SpanId id;
        if (!detail::parseHex(hex, id.m_bytes.data(), id.m_bytes.size()) || !id.isValid()) {
            return zero();
        }
        return id;
    }

    /**
     * @brief 检查 SpanId 是否有效（非全零）
     * @return 有效返回 true，全零返回 false
     */
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return detail::hasNonZeroByte(m_bytes.data(), m_bytes.size());
    }

    /**
     * @brief 将 SpanId 转换为十六进制字符串
     * @return 16 个字符的小写十六进制字符串
     */
    [[nodiscard]] std::string toHex() const {
        std::string hex(kHexLength, '\0');
        static_cast<void>(toHex(hex.data(), hex.size()));
        return hex;
    }

    /**
     * @brief 将 SpanId 格式化为十六进制字符串写入给定缓冲区
     * @param out 输出缓冲区指针
     * @param len 输出缓冲区大小（至少 kHexLength）
     * @return 成功返回 true，缓冲区不足返回 false
     */
    [[nodiscard]] bool toHex(char* out, std::size_t len) const noexcept {
        return detail::formatHex(m_bytes.data(), m_bytes.size(), out, len);
    }

    /**
     * @brief 将 SpanId 格式化为固定大小的十六进制字符数组
     * @return 包含 kHexLength 个字符的数组
     */
    [[nodiscard]] std::array<char, kHexLength> toHexArray() const noexcept {
        std::array<char, kHexLength> hex{};
        static_cast<void>(toHex(hex.data(), hex.size()));
        return hex;
    }

    /**
     * @brief 获取底层字节数组的只读引用
     * @return 8 字节数组的常量引用
     */
    [[nodiscard]] constexpr const Bytes& bytes() const noexcept {
        return m_bytes;
    }

    friend constexpr bool operator==(const SpanId&, const SpanId&) noexcept = default;

private:
    friend SpanId detail::makeRandomSpanId() noexcept;

    explicit constexpr SpanId(Bytes bytes) noexcept
        : m_bytes(bytes) {
    }

    Bytes m_bytes{}; ///< 底层字节数据
};

} // namespace galay::tracing
