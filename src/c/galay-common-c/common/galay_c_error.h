#ifndef GALAY_C_COMMON_GALAY_C_ERROR_H
#define GALAY_C_COMMON_GALAY_C_ERROR_H

#include "galay_c_defs.h"

#define GALAY_C_VERSION_MAJOR 6u
#define GALAY_C_VERSION_MINOR 0u
#define GALAY_C_VERSION_PATCH 0u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_status_t {
    GALAY_OK = 0,
    GALAY_INVALID_ARGUMENT = 1,
    GALAY_NOT_FOUND = 2,
    GALAY_OUT_OF_MEMORY = 3,
    GALAY_PROTOCOL_ERROR = 4,
    GALAY_UNSUPPORTED = 5,
    GALAY_IO_ERROR = 6,
    GALAY_INTERNAL_ERROR = 7
} galay_status_t;

uint32_t galay_c_version_major( void );
uint32_t galay_c_version_minor( void );
uint32_t galay_c_version_patch( void );
const char* galay_status_string(galay_status_t status);
const char* galay_c_common_get_error(galay_status_t status);

#ifdef __cplusplus
}
#endif

#endif
