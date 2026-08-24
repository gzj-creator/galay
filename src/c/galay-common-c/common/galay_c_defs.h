#ifndef GALAY_C_COMMON_GALAY_C_DEFS_H
#define GALAY_C_COMMON_GALAY_C_DEFS_H

#include "macro.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @file galay_c_defs.h
 * @brief Galay C ABI 的基础类型定义。
 *
 * @details 该头文件只包含跨 C 模块共享的轻量值类型和 ABI 标记，不拥有任何
 * 运行时资源。所有定义均可在 C 和 C++ 编译单元中包含。
 */

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
