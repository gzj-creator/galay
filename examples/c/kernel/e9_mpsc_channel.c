#include <galay/c/galay-kernel-c/concurrency-c/mpsc_channel_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>

typedef struct BatchState {
    galay_kernel_mpsc_channel_t* channel;
    C_IOResult result;
    C_MpscChannelMessage messages[8];
    size_t count;
} BatchState;

static void recv_batch_entry(void* ctx)
{
    BatchState* state = (BatchState*)ctx;
    state->result =
        galay_kernel_mpsc_channel_recv_batch(state->channel,
                                             state->messages,
                                             8,
                                             &state->count,
                                             2000);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_mpsc_channel_t channel = {0};
    galay_coro_task_t task = {0};
    BatchState state;
    state.channel = &channel;
    state.result.code = C_IOResultError;
    state.count = 0;

    char first[] = "alpha";
    char second[] = "beta";
    char third[] = "gamma";
    C_MpscChannelMessage messages[3] = {
        {first, sizeof(first), 0},
        {second, sizeof(second), 0},
        {third, sizeof(third), 0},
    };

    int exit_code = 0;
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_mpsc_channel_create(&channel) != C_MpscChannelSuccess ||
        galay_kernel_mpsc_channel_send_batch(&channel, messages, 3) != C_MpscChannelSuccess ||
        galay_coro_spawn(&runtime, recv_batch_entry, &state, 0, &task).code != C_IOResultOk ||
        galay_coro_join(&task, 3000).code != C_IOResultOk) {
        exit_code = 1;
        goto cleanup;
    }

    if (state.result.code != C_IOResultOk ||
        state.count != 3) {
        exit_code = 2;
        goto cleanup;
    }

    if (printf("mpsc_channel batch count=%zu values=%s,%s,%s\n",
               state.count,
               (char*)state.messages[0].data,
               (char*)state.messages[1].data,
               (char*)state.messages[2].data) < 0) {
        exit_code = 7;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 3;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_mpsc_channel_destroy(&channel) != C_MpscChannelSuccess && exit_code == 0) {
            exit_code = 4;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 5;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 6;
        }
    }
    return exit_code;
}
