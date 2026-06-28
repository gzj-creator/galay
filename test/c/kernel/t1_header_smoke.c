#include <galay/c/galay-kernel-c/async-c/aio_file_c.h>
#include <galay/c/galay-kernel-c/async-c/async_file_c.h>
#include <galay/c/galay-kernel-c/async-c/file_watcher_c.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/async_mutex_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/mpsc_channel_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_wait_c.h>

int main(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t tcp = {0};
    galay_kernel_udp_socket_t udp = {0};
    galay_kernel_async_file_t async_file = {0};
    galay_kernel_aio_file_t aio_file = {0};
    galay_kernel_file_watcher_t file_watcher = {0};
    galay_kernel_async_mutex_t async_mutex = {0};
    galay_kernel_async_waiter_t async_waiter = {0};
    galay_kernel_mpsc_channel_t mpsc_channel = {0};
    galay_kernel_unsafe_channel_t unsafe_channel = {0};
    galay_coro_task_t coro_task = {0};
    C_CoroWaitRequest wait_request = {0};
    C_CoroWaitEventToken wait_token = {0};
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
        C_TcpSocketOperationInvalid
    };
    C_UdpSocketResultCode udp_code = C_UdpSocketSuccess;
    C_UdpSocketResultCode udp_timeout_code = C_UdpSocketTimeout;
    C_AsyncFileResultCode async_file_code = C_AsyncFileSuccess;
    C_AsyncFileResultCode async_file_timeout_code = C_AsyncFileTimeout;
    C_AsyncFileOpenMode async_file_mode = C_AsyncFileOpenModeReadWrite;
    C_AioFileResultCode aio_file_code = C_AioFileSuccess;
    C_AioFileOpenMode aio_file_mode = C_AioFileOpenModeReadWrite;
    C_FileWatcherResultCode file_watcher_code = C_FileWatcherSuccess;
    C_FileWatcherResultCode file_watcher_timeout_code = C_FileWatcherTimeout;
    C_FileWatchEvent file_watch_event = C_FileWatchEventModify;
    C_AsyncMutexResultCode async_mutex_code = C_AsyncMutexSuccess;
    C_AsyncWaiterResultCode async_waiter_code = C_AsyncWaiterSuccess;
    C_MpscChannelResultCode mpsc_code = C_MpscChannelSuccess;
    C_MpscChannelMessage mpsc_message = {0};
    C_UnsafeChannelResultCode unsafe_code = C_UnsafeChannelSuccess;
    C_UnsafeChannelMessage unsafe_message = {0};
    C_UnsafeChannelWakeMode unsafe_wake_mode = C_UnsafeChannelWakeModeDeferred;

    return runtime.runtime == 0 &&
            tcp.socket == 0 &&
            udp.socket == 0 &&
            async_file.file == 0 &&
            aio_file.file == 0 &&
            file_watcher.watcher == 0 &&
            async_mutex.mutex == 0 &&
            async_waiter.waiter == 0 &&
            mpsc_channel.channel == 0 &&
            unsafe_channel.channel == 0 &&
            coro_task.task == 0 &&
            wait_request.request == 0 &&
            wait_token.token == 0 &&
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
            udp_code == C_UdpSocketSuccess &&
            udp_timeout_code == C_UdpSocketTimeout &&
            async_file_code == C_AsyncFileSuccess &&
            async_file_timeout_code == C_AsyncFileTimeout &&
            async_file_mode == C_AsyncFileOpenModeReadWrite &&
            aio_file_code == C_AioFileSuccess &&
            aio_file_mode == C_AioFileOpenModeReadWrite &&
            file_watcher_code == C_FileWatcherSuccess &&
            file_watcher_timeout_code == C_FileWatcherTimeout &&
            file_watch_event == C_FileWatchEventModify &&
            async_mutex_code == C_AsyncMutexSuccess &&
            async_waiter_code == C_AsyncWaiterSuccess &&
            mpsc_code == C_MpscChannelSuccess &&
            mpsc_message.data == 0 &&
            unsafe_code == C_UnsafeChannelSuccess &&
            unsafe_message.data == 0 &&
            unsafe_wake_mode == C_UnsafeChannelWakeModeDeferred
        ? 0
        : 1;
}
