#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

int main(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t tcp = {0};
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    C_Host tcp_host = {
        IPV4,
        "127.0.0.1",
        0
    };

    return runtime.runtime == 0 &&
            tcp.socket == 0 &&
            runtime_config.io_scheduler_count == C_RUNTIME_SCHEDULER_COUNT_AUTO &&
            tcp_host.type == IPV4 &&
            tcp_host.port == 0
        ? 0
        : 1;
}
