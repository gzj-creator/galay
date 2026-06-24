#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

int main(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t tcp = {0};
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    C_Host tcp_host = {
        C_IPTypeIPV4,
        "127.0.0.1",
        0
    };
    C_IPType ipv4_type = C_IPTypeIPV4;
    C_IPType ipv6_type = C_IPTypeIPV6;
    C_TcpSocketResultCode codes[] = {
        C_TcpSocketSuccess,
        C_TcpSocketParameterInvalid,
        C_TcpSocketMemoryAllocFailed,
        C_TcpSocketIOFailed,
        C_TcpSocketOperationInvalid,
        C_TcpSocketRuntimeNotRunning,
        C_TcpSocketRuntimeSpawnFailed
    };

    return runtime.runtime == 0 &&
            tcp.socket == 0 &&
            runtime_config.io_scheduler_count == C_RUNTIME_SCHEDULER_COUNT_AUTO &&
            tcp_host.type == C_IPTypeIPV4 &&
            ipv4_type == C_IPTypeIPV4 &&
            ipv6_type == C_IPTypeIPV6 &&
            tcp_host.port == 0 &&
            codes[0] == C_TcpSocketSuccess &&
            codes[1] == C_TcpSocketParameterInvalid &&
            codes[2] == C_TcpSocketMemoryAllocFailed &&
            codes[3] == C_TcpSocketIOFailed &&
            codes[4] == C_TcpSocketOperationInvalid &&
            codes[5] == C_TcpSocketRuntimeNotRunning &&
            codes[6] == C_TcpSocketRuntimeSpawnFailed
        ? 0
        : 1;
}
