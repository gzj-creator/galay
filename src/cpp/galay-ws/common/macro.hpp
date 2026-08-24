/**
 * @file macro.hpp
 * @brief galay-ws 编译期宏集中定义
 */

#ifndef GALAY_WS_COMMON_MACRO_HPP
#define GALAY_WS_COMMON_MACRO_HPP

/// WebSocket frame masking SIMD capability.
#if !defined(GALAY_WS_SIMD_X86) && !defined(GALAY_WS_SIMD_NEON)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define GALAY_WS_SIMD_X86 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
#define GALAY_WS_SIMD_NEON 1
#endif
#endif

#endif  // GALAY_WS_COMMON_MACRO_HPP
