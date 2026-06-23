#include <stdint.h>

#include <galay/c/galay-c/common/galay_c_error.h>

#if !defined(GALAY_C_VERSION_MAJOR)
#error "GALAY_C_VERSION_MAJOR must be visible from C"
#endif

#if !defined(GALAY_C_VERSION_MINOR)
#error "GALAY_C_VERSION_MINOR must be visible from C"
#endif

#if !defined(GALAY_C_VERSION_PATCH)
#error "GALAY_C_VERSION_PATCH must be visible from C"
#endif

int main(void)
{
    const uint32_t major = galay_c_version_major();
    const uint32_t minor = galay_c_version_minor();
    const uint32_t patch = galay_c_version_patch();

    if (major != GALAY_C_VERSION_MAJOR || minor != GALAY_C_VERSION_MINOR ||
        patch != GALAY_C_VERSION_PATCH) {
        return 1;
    }

    return galay_status_string(GALAY_OK) != 0 ? 0 : 2;
}
