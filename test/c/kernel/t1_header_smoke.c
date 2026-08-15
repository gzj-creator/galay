#include <galay/c/galay-kernel-c/kernel.h>

int main(void)
{
    galay_c_runtime_t runtime = {0};
    galay_c_tcp_socket_t tcp = {.fd = -1};
    galay_c_udp_socket_t udp = {.fd = -1};
    galay_c_async_file_t async_file = {.fd = -1};
    galay_c_aio_file_t aio_file = {.fd = -1};
    galay_c_file_watcher_t file_watcher = {.fd = -1};
    galay_c_async_mutex_t async_mutex = {0};
    galay_c_async_waiter_t async_waiter = {0};
    galay_c_mpmc_bounded_channel_t mpmc = {0};
    galay_c_mpsc_bounded_channel_t mpsc = {0};
    galay_c_spsc_bounded_channel_t spsc = {0};
    galay_c_coro_task_t coro_task = {0};
    C_RuntimeConfig runtime_config = galay_c_runtime_config_default();
    C_Host tcp_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    C_Host udp_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    galay_c_file_event_t file_event = {0};
    galay_c_channel_message_t message = {0};
    C_IOResult result = {C_IOResultOk, 0, 0, 0, NULL};

    return runtime.runtime == NULL &&
                   tcp.fd == -1 && udp.fd == -1 &&
                   async_file.fd == -1 && aio_file.fd == -1 &&
                   file_watcher.fd == -1 &&
                   async_mutex.mutex == NULL && async_waiter.waiter == NULL &&
                   mpmc.buffer == NULL && mpsc.buffer == NULL && spsc.buffer == NULL &&
                   coro_task.task == NULL &&
                   runtime_config.io_scheduler_count == C_RUNTIME_SCHEDULER_COUNT_AUTO &&
                   tcp_host.type == C_IPTypeIPV4 && tcp_host.port == 0 &&
                   udp_host.type == C_IPTypeIPV4 && file_event.mask == 0 &&
                   message.data == NULL && result.code == C_IOResultOk
               ? 0
               : 1;
}
