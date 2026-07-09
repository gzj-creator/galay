#include <galay/c/galay-kernel-c/kernel_c.h>

#include <string.h>

int main(void)
{
    if (strcmp(galay_coro_ioresult_get_error(C_IOResultOk), "ok") != 0) {
        return 1;
    }
    if (strcmp(galay_coro_ioresult_get_error(C_IOResultEof), "eof") != 0) {
        return 2;
    }
    if (strcmp(galay_coro_ioresult_get_error(C_IOResultTimeout), "timeout") != 0) {
        return 3;
    }
    if (strcmp(galay_coro_ioresult_get_error(C_IOResultCancelled), "cancelled") != 0) {
        return 4;
    }
    if (strcmp(galay_coro_ioresult_get_error(C_IOResultInvalid), "invalid") != 0) {
        return 5;
    }
    if (strcmp(galay_coro_ioresult_get_error(C_IOResultError), "error") != 0) {
        return 6;
    }
    if (strcmp(galay_coro_ioresult_get_error((C_IOResultCode)1000), "unknown") != 0) {
        return 7;
    }
    return 0;
}
