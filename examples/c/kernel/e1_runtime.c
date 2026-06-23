#include <galay/c/galay-kernel-c/core-c/runtime_c.h>

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 1;

    galay_kernel_runtime_t runtime = {0};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        (void)galay_kernel_runtime_destroy(&runtime);
        return 2;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess) {
        (void)galay_kernel_runtime_destroy(&runtime);
        return 3;
    }
    return galay_kernel_runtime_destroy(&runtime) == C_RuntimeSuccess ? 0 : 4;
}
