#include <galay/c/galay-common-c/common/galay_c_error.h>

extern "C" {

uint32_t galay_c_version_major( void )
{
    return GALAY_C_VERSION_MAJOR;
}

uint32_t galay_c_version_minor( void )
{
    return GALAY_C_VERSION_MINOR;
}

uint32_t galay_c_version_patch( void )
{
    return GALAY_C_VERSION_PATCH;
}

const char* galay_status_string(galay_status_t status)
{
    switch (status) {
        case GALAY_OK:
            return "ok";
        case GALAY_INVALID_ARGUMENT:
            return "invalid argument";
        case GALAY_NOT_FOUND:
            return "not found";
        case GALAY_OUT_OF_MEMORY:
            return "out of memory";
        case GALAY_PROTOCOL_ERROR:
            return "protocol error";
        case GALAY_UNSUPPORTED:
            return "unsupported";
        case GALAY_IO_ERROR:
            return "io error";
        case GALAY_INTERNAL_ERROR:
            return "internal error";
    }
    return "unknown";
}

const char* galay_c_common_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

}
