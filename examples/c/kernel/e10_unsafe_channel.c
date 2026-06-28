#include <galay/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>

typedef struct RecvState {
    galay_kernel_unsafe_channel_t* channel;
    C_IOResult result;
    C_UnsafeChannelMessage message;
    int value;
} RecvState;

typedef struct ProducerCtx {
    galay_kernel_unsafe_channel_t* channel;
    C_UnsafeChannelMessage message;
    C_UnsafeChannelResultCode result;
} ProducerCtx;

static void recv_entry(void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    state->result =
        galay_kernel_unsafe_channel_recv(state->channel, &state->message, 2000);
    if (state->result.code == C_IOResultOk && state->message.data != 0) {
        state->value = *(int*)state->message.data;
    }
}

static void producer_entry(void* ctx)
{
    ProducerCtx* state = (ProducerCtx*)ctx;
    C_IOResult yielded = galay_coro_yield();
    if (yielded.code != C_IOResultOk) {
        state->result = C_UnsafeChannelOperationInvalid;
        return;
    }
    state->result = galay_kernel_unsafe_channel_send(state->channel, &state->message);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    galay_coro_task_t recv_task = {0};
    galay_coro_task_t producer_task = {0};
    int payload = 42;
    int exit_code = 0;

    RecvState recv_state;
    recv_state.channel = &channel;
    recv_state.result.code = C_IOResultError;
    recv_state.message = (C_UnsafeChannelMessage){0};
    recv_state.value = 0;

    ProducerCtx producer;
    producer.channel = &channel;
    producer.message.data = &payload;
    producer.message.size = sizeof(payload);
    producer.message.user = 0;
    producer.result = C_UnsafeChannelIOFailed;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline) != C_UnsafeChannelSuccess) {
        return 1;
    }

    if (galay_coro_spawn(&runtime, recv_entry, &recv_state, 0, &recv_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, producer_entry, &producer, 0, &producer_task).code != C_IOResultOk ||
        galay_coro_join(&producer_task, 3000).code != C_IOResultOk ||
        galay_coro_join(&recv_task, 3000).code != C_IOResultOk ||
        producer.result != C_UnsafeChannelSuccess ||
        recv_state.result.code != C_IOResultOk) {
        exit_code = 2;
        goto cleanup;
    }

    if (printf("unsafe_channel received value=%d empty=%d\n",
               recv_state.value,
               galay_kernel_unsafe_channel_empty(&channel) ? 1 : 0) < 0) {
        exit_code = 8;
        goto cleanup;
    }

cleanup:
    if (producer_task.task != 0) {
        if (galay_coro_destroy(&producer_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 3;
        }
    }
    if (recv_task.task != 0) {
        if (galay_coro_destroy(&recv_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 4;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_unsafe_channel_destroy(&channel) != C_UnsafeChannelSuccess && exit_code == 0) {
            exit_code = 5;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 6;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 7;
        }
    }
    return exit_code;
}
