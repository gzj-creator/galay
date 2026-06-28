#include <galay/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

enum {
    UNSAFE_CHANNEL_MESSAGES = 100000
};

typedef struct RecvState {
    galay_kernel_unsafe_channel_t* channel;
    C_IOResult result;
    C_UnsafeChannelMessage* received;
    size_t count;
    uint64_t sum;
} RecvState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void recv_entry(void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    state->result =
        galay_kernel_unsafe_channel_recv_batched(state->channel,
                                                 UNSAFE_CHANNEL_MESSAGES,
                                                 state->received,
                                                 UNSAFE_CHANNEL_MESSAGES,
                                                 &state->count,
                                                 10000);
    if (state->result.code == C_IOResultOk) {
        uint64_t sum = 0;
        for (size_t i = 0; i < state->count; ++i) {
            sum += *(uint64_t*)state->received[i].data;
        }
        state->sum = sum;
    }
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    galay_coro_task_t recv_task = {0};
    uint64_t* payloads = 0;
    C_UnsafeChannelMessage* messages = 0;
    C_UnsafeChannelMessage* received = 0;
    int exit_code = 0;

    RecvState recv_state;
    recv_state.channel = &channel;
    recv_state.result.code = C_IOResultError;
    recv_state.received = 0;
    recv_state.count = 0;
    recv_state.sum = 0;

    payloads = (uint64_t*)malloc(sizeof(uint64_t) * UNSAFE_CHANNEL_MESSAGES);
    messages = (C_UnsafeChannelMessage*)malloc(sizeof(C_UnsafeChannelMessage) * UNSAFE_CHANNEL_MESSAGES);
    received = (C_UnsafeChannelMessage*)malloc(sizeof(C_UnsafeChannelMessage) * UNSAFE_CHANNEL_MESSAGES);
    if (payloads == 0 || messages == 0 || received == 0) {
        exit_code = 1;
        goto cleanup;
    }
    for (size_t i = 0; i < UNSAFE_CHANNEL_MESSAGES; ++i) {
        payloads[i] = (uint64_t)i;
        messages[i].data = &payloads[i];
        messages[i].size = sizeof(payloads[i]);
        messages[i].user = 0;
    }
    recv_state.received = received;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline) != C_UnsafeChannelSuccess ||
        galay_coro_spawn(&runtime, recv_entry, &recv_state, 0, &recv_task).code != C_IOResultOk) {
        exit_code = 2;
        goto cleanup;
    }

    const int64_t start = now_us();
    C_UnsafeChannelResultCode sent =
        galay_kernel_unsafe_channel_send_batch(&channel, messages, UNSAFE_CHANNEL_MESSAGES);
    if (sent != C_UnsafeChannelSuccess ||
        galay_coro_join(&recv_task, 12000).code != C_IOResultOk) {
        exit_code = 3;
        goto cleanup;
    }
    const int64_t elapsed = now_us() - start;

    const uint64_t expected_sum =
        ((uint64_t)(UNSAFE_CHANNEL_MESSAGES - 1) * (uint64_t)UNSAFE_CHANNEL_MESSAGES) / 2;
    if (recv_state.result.code != C_IOResultOk ||
        recv_state.count != UNSAFE_CHANNEL_MESSAGES ||
        recv_state.sum != expected_sum ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 4;
        goto cleanup;
    }

    {
        const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
        const double ops_per_sec = seconds > 0.0
            ? (double)(UNSAFE_CHANNEL_MESSAGES * 2) / seconds
            : 0.0;
        if (printf("unsafe_channel_throughput scheduler_count=1 messages=%d elapsed_ms=%.3f ops_per_sec=%.2f\n",
                   UNSAFE_CHANNEL_MESSAGES,
                   (double)elapsed / 1000.0,
                   ops_per_sec) < 0) {
            exit_code = 5;
        }
    }

cleanup:
    if (recv_task.task != 0) {
        if (galay_coro_destroy(&recv_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 5;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_unsafe_channel_destroy(&channel) != C_UnsafeChannelSuccess && exit_code == 0) {
            exit_code = 6;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 7;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 8;
        }
    }
    free(received);
    free(messages);
    free(payloads);
    return exit_code;
}
