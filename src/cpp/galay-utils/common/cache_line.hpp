/**
 * @file cache_line.hpp
 * @brief 并发结构使用的 ABI 稳定缓存行常量
 */

#ifndef GALAY_UTILS_COMMON_CACHE_LINE_HPP
#define GALAY_UTILS_COMMON_CACHE_LINE_HPP

#include "macro.hpp"

#include <cstddef>

namespace galay::utils {

/**
 * @brief 公开并发数据结构使用的 ABI 稳定缓存行隔离大小。
 * @details AArch64 使用 128 字节，其他架构使用 64 字节；固定编译期值避免
 *          std::hardware_destructive_interference_size 随编译参数改变公开布局。
 */
#if defined(GALAY_ARCH_ARM64)
inline constexpr size_t kCacheLineSize = 128;
#else
inline constexpr size_t kCacheLineSize = 64;
#endif

}  // namespace galay::utils

#endif  // GALAY_UTILS_COMMON_CACHE_LINE_HPP
