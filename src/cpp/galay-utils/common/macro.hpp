/**
 * @file macro.hpp
 * @brief galay-utils 编译期宏集中定义
 *
 * 平台、架构、编译器和可选系统能力只在此处探测。实现头文件只消费
 * 这些宏，不再各自重复探测或定义配置。
 */

#ifndef GALAY_UTILS_COMMON_MACRO_HPP
#define GALAY_UTILS_COMMON_MACRO_HPP

/// 平台检测宏。
#if defined(__APPLE__)
#define GALAY_PLATFORM_MACOS 1
#elif defined(__linux__)
#define GALAY_PLATFORM_LINUX 1
#elif defined(_WIN32) || defined(_WIN64)
#define GALAY_PLATFORM_WINDOWS 1
#endif

/// 架构检测宏。
#if defined(__aarch64__) || defined(__ARM_ARCH_ISA_A64) || \
    defined(_M_ARM64) || defined(_M_ARM64EC)
#define GALAY_ARCH_ARM64 1
#elif defined(__x86_64__) || defined(_M_X64)
#define GALAY_ARCH_X64 1
#elif defined(__i386__) || defined(_M_IX86)
#define GALAY_ARCH_X86 1
#endif

/// 编译器检测宏。
#if defined(__clang__)
#define GALAY_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define GALAY_COMPILER_GCC 1
#elif defined(_MSC_VER)
#define GALAY_COMPILER_MSVC 1
#endif

/// 通用编译器辅助宏。
#if defined(GALAY_COMPILER_GCC) || defined(GALAY_COMPILER_CLANG)
#define GALAY_LIKELY(x) __builtin_expect(!!(x), 1)
#define GALAY_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define GALAY_LIKELY(x) (x)
#define GALAY_UNLIKELY(x) (x)
#endif

#if defined(GALAY_COMPILER_MSVC)
#define GALAY_FORCE_INLINE __forceinline
#else
#define GALAY_FORCE_INLINE __attribute__((always_inline)) inline
#endif

#define GALAY_UNUSED(x) (void)(x)

/// RingBuffer 可选 POSIX 后端能力。
#if defined(__unix__) || defined(__APPLE__)
#define GALAY_UTILS_RING_BUFFER_HAS_IOVEC 1
#define GALAY_UTILS_RING_BUFFER_HAS_MMAP 1
#else
#define GALAY_UTILS_RING_BUFFER_HAS_IOVEC 0
#define GALAY_UTILS_RING_BUFFER_HAS_MMAP 0
#endif

#if defined(__linux__) && !defined(MFD_CLOEXEC)
#define MFD_CLOEXEC 0x0001U
#endif

#endif  // GALAY_UTILS_COMMON_MACRO_HPP
