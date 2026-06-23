#include <galay/c/galay-kernel-c/core-c/runtime_c.h>

static int expect_status(C_RuntimeResultCode actual, C_RuntimeResultCode expected)
{
    return actual == expected ? 0 : 1;
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 1;

    galay_kernel_runtime_t runtime = {0};
    if (expect_status(galay_kernel_runtime_create(&config, &runtime), C_RuntimeSuccess)) {
        return 1;
    }
    if (runtime.runtime == 0 || galay_kernel_runtime_is_running(&runtime)) {
        return 2;
    }
    if (expect_status(galay_kernel_runtime_stop(&runtime), C_RuntimeSuccess)) {
        return 3;
    }
    if (expect_status(galay_kernel_runtime_start(&runtime), C_RuntimeSuccess)) {
        return 4;
    }
    if (!galay_kernel_runtime_is_running(&runtime)) {
        return 5;
    }
    if (expect_status(galay_kernel_runtime_stop(&runtime), C_RuntimeSuccess)) {
        return 7;
    }
    if (galay_kernel_runtime_is_running(&runtime)) {
        return 8;
    }
    if (expect_status(galay_kernel_runtime_start(0), C_RuntimeParameterInvalid)) {
        return 9;
    }
    if (expect_status(galay_kernel_runtime_stop(0), C_RuntimeParameterInvalid)) {
        return 10;
    }
    if (expect_status(galay_kernel_runtime_destroy(&runtime), C_RuntimeSuccess)) {
        return 11;
    }
    if (runtime.runtime != 0) {
        return 12;
    }
    if (expect_status(galay_kernel_runtime_destroy(0), C_RuntimeParameterInvalid)) {
        return 14;
    }
    return 0;
}
