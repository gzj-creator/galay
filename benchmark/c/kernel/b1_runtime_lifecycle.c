#include <galay/c/galay-kernel-c/core-c/runtime_c.h>

#include <stdio.h>

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 1;

    int completed = 0;
    for (int i = 0; i < 16; ++i) {
        galay_kernel_runtime_t runtime = {0};
        if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
            return 1;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
            return 2;
        }
        ++completed;
    }

    printf("kernel C runtime lifecycle smoke iterations=%d\n", completed);
    return completed == 16 ? 0 : 3;
}
