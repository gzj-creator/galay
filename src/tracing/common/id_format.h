/**
 * @file id_format.h
 * @brief 追踪标识符的十六进制解析、格式化与随机生成工具
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 提供 TraceId / SpanId 所需的底层十六进制转换、校验及随机生成函数，
 * 仅供库内部 detail 命名空间使用。
 */

#pragma once

#include <cstddef>
#include <string_view>

namespace galay::tracing {

class SpanId;
class TraceId;

namespace detail {

/**
 * @brief 将单个十六进制字符转换为对应的整数值
 * @param ch 十六进制字符（0-9、a-f、A-F）
 * @return 对应的整数值（0-15），无效字符返回 -1
 */
[[nodiscard]] constexpr int hexValue(char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/**
 * @brief 检查字节数组中是否包含非零字节
 * @param bytes 字节数组指针
 * @param len 数组长度
 * @return 存在非零字节时返回 true，全为零时返回 false
 */
[[nodiscard]] constexpr bool hasNonZeroByte(const std::byte* bytes, std::size_t len) noexcept {
    for (std::size_t i = 0; i < len; ++i) {
        if (bytes[i] != std::byte{0}) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 将十六进制字符串解析为字节数组
 * @param hex 十六进制字符串视图
 * @param out 输出字节数组指针
 * @param len 期望解析的字节数
 * @return 解析成功返回 true，格式不匹配或长度不足时返回 false
 */
[[nodiscard]] bool parseHex(std::string_view hex, std::byte* out, std::size_t len) noexcept;

/**
 * @brief 将字节数组格式化为小写十六进制字符串
 * @param bytes 输入字节数组指针
 * @param byteLen 输入字节长度
 * @param out 输出字符缓冲区指针
 * @param outLen 输出缓冲区大小（至少为 byteLen * 2）
 * @return 格式化成功返回 true，缓冲区不足时返回 false
 */
[[nodiscard]] bool formatHex(const std::byte* bytes, std::size_t byteLen, char* out, std::size_t outLen) noexcept;

/**
 * @brief 生成随机的 TraceId（16 字节）
 * @return 随机生成的 TraceId
 */
[[nodiscard]] TraceId makeRandomTraceId() noexcept;

/**
 * @brief 生成随机的 SpanId（8 字节）
 * @return 随机生成的 SpanId
 */
[[nodiscard]] SpanId makeRandomSpanId() noexcept;

} // namespace detail
} // namespace galay::tracing
