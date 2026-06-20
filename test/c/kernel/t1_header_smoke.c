#include <galay/c/galay-kernel/galay_kernel.h>

int main(void)
{
    galay_kernel_runtime_t* runtime = 0;
    galay_kernel_tcp_socket_t* tcp = 0;
    galay_kernel_udp_socket_t* udp = 0;
    galay_kernel_runtime_config_t runtime_config = galay_kernel_runtime_config_default();
    galay_kernel_tcp_host_config_t tcp_host = {
        GALAY_KERNEL_IP_V4,
        "127.0.0.1",
        0
    };
    galay_kernel_udp_host_config_t udp_host = {
        GALAY_KERNEL_IP_V6,
        "::1",
        0
    };

    return runtime == 0 &&
            tcp == 0 &&
            udp == 0 &&
            runtime_config.io_scheduler_count == GALAY_KERNEL_SCHEDULER_COUNT_AUTO &&
            tcp_host.port == 0 &&
            udp_host.port == 0
        ? 0
        : 1;
}
