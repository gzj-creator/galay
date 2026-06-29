#ifndef GALAY_C_COMMON_GALAY_C_DEFS_H
#define GALAY_C_COMMON_GALAY_C_DEFS_H

#include <stddef.h>
#include <stdint.h>

#define GALAY_C_API 1

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_bool_t {
    GALAY_FALSE = 0,
    GALAY_TRUE = 1
} galay_bool_t;

#ifdef __cplusplus
}
#endif

#endif
