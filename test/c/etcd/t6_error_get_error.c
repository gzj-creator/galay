#include <galay/c/galay-etcd-c/etcd_c.h>

#include <string.h>

int main(void)
{
    const galay_etcd_error_code_t codes[] = {
        GALAY_ETCD_ERROR_SUCCESS,
        GALAY_ETCD_ERROR_INVALID_ENDPOINT,
        GALAY_ETCD_ERROR_INVALID_ARGUMENT,
        GALAY_ETCD_ERROR_NOT_CONNECTED,
        GALAY_ETCD_ERROR_IO,
        GALAY_ETCD_ERROR_PROTOCOL,
        GALAY_ETCD_ERROR_CANCELLED,
    };
    const size_t count = sizeof(codes) / sizeof(codes[0]);
    for (size_t i = 0; i < count; ++i) {
        const char* text = galay_etcd_get_error(codes[i]);
        if (text == 0 || text[0] == '\0' || strcmp(text, "unknown") == 0) {
            return (int)i + 1;
        }
    }
    if (strcmp(galay_etcd_get_error((galay_etcd_error_code_t)1000), "unknown") != 0) {
        return 100;
    }
    return 0;
}
