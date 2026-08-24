/**
 * @file macro.h
 * @brief galay C ABI 编译期版本宏
 */

#ifndef GALAY_C_COMMON_MACRO_H
#define GALAY_C_COMMON_MACRO_H

#include <stddef.h>

/**
 * @brief Galay C ABI 可用性标记。
 * @note 该宏表示当前头文件提供 C ABI 声明，不表达运行时版本号。
 */
#ifndef GALAY_C_API
#define GALAY_C_API 1
#endif

#ifdef __cplusplus
#define GALAY_C_ATOMIC(type) type
#define GALAY_C_ALIGNAS(value) alignas(value)
#else
#define GALAY_C_ATOMIC(type) _Atomic(type)
#define GALAY_C_ALIGNAS(value) _Alignas(value)
#endif

/** @brief 当前 C ABI 头文件主版本号。 */
#ifndef GALAY_C_VERSION_MAJOR
#define GALAY_C_VERSION_MAJOR 4u
#endif
/** @brief 当前 C ABI 头文件次版本号。 */
#ifndef GALAY_C_VERSION_MINOR
#define GALAY_C_VERSION_MINOR 0u
#endif
/** @brief 当前 C ABI 头文件修订版本号。 */
#ifndef GALAY_C_VERSION_PATCH
#define GALAY_C_VERSION_PATCH 0u
#endif

#endif  /* GALAY_C_COMMON_MACRO_H */
