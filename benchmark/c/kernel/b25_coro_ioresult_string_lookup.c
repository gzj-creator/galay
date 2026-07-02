#include <galay/c/galay-kernel-c/kernel_c.h>

#include <stdint.h>
#include <stdio.h>
#include <time.h>

enum {
    kIterations = 1000000,
};

static int64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

int main(void)
{
    const C_IOResultCode codes[] = {
        C_IOResultOk,
        C_IOResultEof,
        C_IOResultTimeout,
        C_IOResultCancelled,
        C_IOResultInvalid,
        C_IOResultError,
        (C_IOResultCode)1000,
    };
    volatile uintptr_t checksum = 0;

    const int64_t start = now_ns();
    if (start == 0) {
        return 1;
    }
    for (int i = 0; i < kIterations; ++i) {
        const C_IOResultCode code = codes[i % 7];
        const char* text = galay_coro_ioresult_string(code);
        if (text == 0) {
            return 2;
        }
        checksum += (uintptr_t)text[0] + (uintptr_t)galay_coro_ioresult_to_status(code);
    }
    const int64_t elapsed = now_ns() - start;
    if (elapsed <= 0) {
        return 3;
    }

    printf("coro_ioresult_string_lookup iterations=%d elapsed_ns=%lld avg_ns=%.2f checksum=%llu\n",
           kIterations,
           (long long)elapsed,
           (double)elapsed / (double)kIterations,
           (unsigned long long)checksum);
    return 0;
}
