#include <galay/c/galay-kernel-c/kernel_c.h>

#include <string.h>

int main(void)
{
    if (strcmp(galay_coro_ioresult_string(C_IOResultOk), "ok") != 0) {
        return 1;
    }
    if (strcmp(galay_coro_ioresult_string(C_IOResultEof), "eof") != 0) {
        return 2;
    }
    if (strcmp(galay_coro_ioresult_string(C_IOResultTimeout), "timeout") != 0) {
        return 3;
    }
    if (strcmp(galay_coro_ioresult_string(C_IOResultCancelled), "cancelled") != 0) {
        return 4;
    }
    if (strcmp(galay_coro_ioresult_string(C_IOResultInvalid), "invalid") != 0) {
        return 5;
    }
    if (strcmp(galay_coro_ioresult_string(C_IOResultError), "error") != 0) {
        return 6;
    }
    if (strcmp(galay_coro_ioresult_string((C_IOResultCode)1000), "unknown") != 0) {
        return 7;
    }
    if (galay_coro_ioresult_to_status(C_IOResultOk) != GALAY_OK) {
        return 8;
    }
    if (galay_coro_ioresult_to_status(C_IOResultEof) != GALAY_EOF) {
        return 9;
    }
    if (galay_coro_ioresult_to_status(C_IOResultTimeout) != GALAY_TIMEOUT) {
        return 10;
    }
    if (galay_coro_ioresult_to_status(C_IOResultCancelled) != GALAY_CANCELLED) {
        return 11;
    }
    if (galay_coro_ioresult_to_status(C_IOResultInvalid) != GALAY_INVALID_ARGUMENT) {
        return 12;
    }
    if (galay_coro_ioresult_to_status(C_IOResultError) != GALAY_IO_ERROR) {
        return 13;
    }
    if (galay_coro_ioresult_to_status((C_IOResultCode)1000) != GALAY_INTERNAL_ERROR) {
        return 14;
    }
    return 0;
}
