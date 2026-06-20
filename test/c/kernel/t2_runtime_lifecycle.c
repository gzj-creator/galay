#include <galay/c/galay-kernel/galay_kernel.h>

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    return actual == expected ? 0 : 1;
}

int main(void)
{
    galay_kernel_runtime_config_t config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 1;

    galay_kernel_runtime_t* runtime = 0;
    if (expect_status(galay_kernel_runtime_create(&config, &runtime), GALAY_OK)) {
        return 1;
    }
    if (runtime == 0 || galay_kernel_runtime_is_running(runtime) != GALAY_FALSE) {
        return 2;
    }
    if (expect_status(galay_kernel_runtime_stop(runtime), GALAY_OK)) {
        return 3;
    }
    if (expect_status(galay_kernel_runtime_start(runtime), GALAY_OK)) {
        return 4;
    }
    if (galay_kernel_runtime_is_running(runtime) != GALAY_TRUE) {
        return 5;
    }
    if (expect_status(galay_kernel_runtime_start(runtime), GALAY_OK)) {
        return 6;
    }
    if (expect_status(galay_kernel_runtime_stop(runtime), GALAY_OK)) {
        return 7;
    }
    if (galay_kernel_runtime_is_running(runtime) != GALAY_FALSE) {
        return 8;
    }
    if (expect_status(galay_kernel_runtime_start(0), GALAY_INVALID_ARGUMENT)) {
        return 9;
    }
    if (expect_status(galay_kernel_runtime_stop(0), GALAY_INVALID_ARGUMENT)) {
        return 10;
    }
    if (expect_status(galay_kernel_runtime_destroy(&runtime), GALAY_OK)) {
        return 11;
    }
    if (runtime != 0) {
        return 12;
    }
    if (expect_status(galay_kernel_runtime_destroy(&runtime), GALAY_OK)) {
        return 13;
    }
    if (expect_status(galay_kernel_runtime_destroy(0), GALAY_INVALID_ARGUMENT)) {
        return 14;
    }
    return 0;
}
