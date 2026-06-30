#ifndef GALAY_C_COMMON_GALAY_C_DEFS_H
#define GALAY_C_COMMON_GALAY_C_DEFS_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file galay_c_defs.h
 * @brief Galay C ABI 的基础类型定义。
 *
 * @details 该头文件只包含跨 C 模块共享的轻量值类型和 ABI 标记，不拥有任何
 * 运行时资源。所有定义均可在 C 和 C++ 编译单元中包含。
 */

/**
 * @brief Galay C ABI 可用性标记。
 *
 * @note 该宏用于编译期判断当前头文件提供 C ABI 声明，不表达运行时版本号。
 * 运行时版本请使用 galay_c_version_major/minor/patch。
 */
#define GALAY_C_API 1

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C ABI 布尔值。
 *
 * @details 用固定枚举值避免跨编译器、跨语言边界直接暴露 C++ bool 布局。
 *
 * @note 仅 GALAY_FALSE 和 GALAY_TRUE 是有效值；公开 API 返回该类型时调用方
 * 应按枚举值判断，不要假设其它非零值也代表 true。
 */
typedef enum galay_bool_t {
    GALAY_FALSE = 0,     ///< false。
    GALAY_TRUE = 1       ///< true。
} galay_bool_t;

#ifdef __cplusplus
}
#endif

#endif
