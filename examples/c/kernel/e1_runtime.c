#include <galay/c/galay-kernel/galay_kernel.h>

int main(void)
{
    galay_kernel_runtime_config_t config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 1;

    galay_kernel_runtime_t* runtime = 0;
    if (galay_kernel_runtime_create(&config, &runtime) != GALAY_OK) {
        return 1;
    }
    if (galay_kernel_runtime_start(runtime) != GALAY_OK) {
        (void)galay_kernel_runtime_destroy(&runtime);
        return 2;
    }
    if (galay_kernel_runtime_stop(runtime) != GALAY_OK) {
        (void)galay_kernel_runtime_destroy(&runtime);
        return 3;
    }
    return galay_kernel_runtime_destroy(&runtime) == GALAY_OK ? 0 : 4;
}
