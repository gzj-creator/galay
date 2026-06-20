/**
 * @file galay_c_defs.h
 * @brief Galay C ABI 公共定义。
 */

#ifndef GALAY_C_COMMON_GALAY_C_DEFS_H
#define GALAY_C_COMMON_GALAY_C_DEFS_H

#include <stddef.h>
#include <stdint.h>

#define GALAY_C_VERSION_MAJOR 6
#define GALAY_C_VERSION_MINOR 0
#define GALAY_C_VERSION_PATCH 0

#ifdef __cplusplus
#define GALAY_C_BEGIN_DECLS extern "C" {
#define GALAY_C_END_DECLS }
#else
#define GALAY_C_BEGIN_DECLS
#define GALAY_C_END_DECLS
#endif

#if defined(_WIN32) && defined(GALAY_C_SHARED)
#if defined(GALAY_C_BUILDING)
#define GALAY_C_API __declspec(dllexport)
#else
#define GALAY_C_API __declspec(dllimport)
#endif
#else
#define GALAY_C_API
#endif

GALAY_C_BEGIN_DECLS

typedef uint8_t galay_bool_t;

enum {
    GALAY_FALSE = 0,
    GALAY_TRUE = 1
};

GALAY_C_END_DECLS

#endif /* GALAY_C_COMMON_GALAY_C_DEFS_H */
