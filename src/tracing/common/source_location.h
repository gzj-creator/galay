/**
 * @file source_location.h
 * @brief 源码位置信息结构体
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 提供轻量级的源码位置记录，用于日志和追踪事件中标记调用点的
 * 文件名、行号和函数名。
 */

#pragma once

#include <cstdint>
#include <source_location>

namespace galay::tracing {

/**
 * @brief 源码位置信息
 * @details 记录源代码中的调用位置，包含文件名、行号和函数名。
 * 封装 std::source_location 以提供跨编译器的统一接口。
 */
struct SourceLocation {
    const char* file{""};              ///< 源文件名
    std::uint_least32_t line{0};       ///< 源文件行号
    const char* function{""};          ///< 所在函数名

    /**
     * @brief 获取当前调用点的源码位置
     * @param location 底层 source_location，默认为调用点位置
     * @return 当前调用点的 SourceLocation
     */
    [[nodiscard]] static consteval SourceLocation current(
        const std::source_location location = std::source_location::current()) noexcept {
        return SourceLocation{location.file_name(), location.line(), location.function_name()};
    }
};

} // namespace galay::tracing
