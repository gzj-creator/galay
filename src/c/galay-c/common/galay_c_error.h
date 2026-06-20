/**
 * @file galay_c_error.h
 * @brief Galay C ABI 公共状态码。
 */

#ifndef GALAY_C_COMMON_GALAY_C_ERROR_H
#define GALAY_C_COMMON_GALAY_C_ERROR_H

#include <galay/c/galay-c/common/galay_c_defs.h>

GALAY_C_BEGIN_DECLS

typedef enum galay_status {
    GALAY_OK = 0,
    GALAY_INVALID_ARGUMENT = 1,
    GALAY_OUT_OF_MEMORY = 2,
    GALAY_IO_ERROR = 3,
    GALAY_PROTOCOL_ERROR = 4,
    GALAY_UNSUPPORTED = 5,
    GALAY_NOT_FOUND = 6,
    GALAY_INTERNAL_ERROR = 7
} galay_status_t;

GALAY_C_API const char* galay_status_string(galay_status_t status);

GALAY_C_END_DECLS

#endif /* GALAY_C_COMMON_GALAY_C_ERROR_H */
