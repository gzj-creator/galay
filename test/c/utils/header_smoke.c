#include <galay/c/galay-utils/utils.h>

#if !defined(GALAY_C_API)
#error "utils.h must include the common C ABI definitions"
#endif

#if !defined(GALAY_C_COMMON_GALAY_C_ERROR_H)
#error "utils.h must include the common C status contract"
#endif

#if defined(__clang__) || defined(__GNUC__)
typedef galay_status_t (*galay_utils_bytes_create_common_status_fn)(
    const void*,
    size_t,
    galay_utils_bytes_t**);
typedef galay_status_t (*galay_utils_base64_encode_common_status_fn)(
    const void*,
    size_t,
    char*,
    size_t,
    size_t*);

_Static_assert(
    __builtin_types_compatible_p(
        __typeof__(&galay_utils_bytes_create),
        galay_utils_bytes_create_common_status_fn),
    "galay_utils_bytes_create must return galay_status_t");
_Static_assert(
    __builtin_types_compatible_p(
        __typeof__(&galay_utils_base64_encode),
        galay_utils_base64_encode_common_status_fn),
    "galay_utils_base64_encode must return galay_status_t");
#endif

int main(void)
{
    return GALAY_UTILS_OK == GALAY_OK ? 0 : 1;
}
