#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h>

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

typedef struct RecvState {
    atomic_int done;
    atomic_int code;
    int value;
} RecvState;

typedef struct ProducerCtx {
    galay_kernel_unsafe_channel_t* channel;
    C_UnsafeChannelMessage message;
    atomic_int done;
    atomic_int code;
} ProducerCtx;

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static int wait_waiter_waiting(galay_kernel_async_waiter_t* waiter)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000; ++i) {
        if (galay_kernel_async_waiter_is_waiting(waiter)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static void on_recv(galay_kernel_unsafe_channel_recv_result_t* result, void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    atomic_store(&state->code, (int)result->code);
    if (result->code == C_UnsafeChannelSuccess && result->message.data != 0) {
        state->value = *(int*)result->message.data;
    }
    atomic_store(&state->done, 1);
}

static void on_produce(C_AsyncWaiterResultCode code, void* ctx)
{
    ProducerCtx* producer = (ProducerCtx*)ctx;
    C_UnsafeChannelResultCode result = C_UnsafeChannelIOFailed;
    if (code == C_AsyncWaiterSuccess) {
        result = galay_kernel_unsafe_channel_send(producer->channel, &producer->message);
    }
    atomic_store(&producer->code, (int)result);
    atomic_store(&producer->done, 1);
}

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    galay_kernel_async_waiter_t producer_waiter = {0};
    int payload = 42;
    int exit_code = 0;

    RecvState recv_state;
    atomic_init(&recv_state.done, 0);
    atomic_init(&recv_state.code, (int)C_UnsafeChannelIOFailed);
    recv_state.value = 0;

    ProducerCtx producer;
    producer.channel = &channel;
    producer.message.data = &payload;
    producer.message.size = sizeof(payload);
    producer.message.user = 0;
    atomic_init(&producer.done, 0);
    atomic_init(&producer.code, (int)C_UnsafeChannelIOFailed);

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline) != C_UnsafeChannelSuccess) {
        return 1;
    }

    if (galay_kernel_unsafe_channel_recv(&runtime, &channel, on_recv, &recv_state) != C_UnsafeChannelSuccess ||
        galay_kernel_async_waiter_create(&producer_waiter) != C_AsyncWaiterSuccess ||
        galay_kernel_async_waiter_wait(&runtime, &producer_waiter, on_produce, &producer) != C_AsyncWaiterSuccess ||
        wait_waiter_waiting(&producer_waiter) != 0 ||
        galay_kernel_async_waiter_notify(&producer_waiter) != C_AsyncWaiterSuccess ||
        wait_done(&producer.done) != 0 ||
        atomic_load(&producer.code) != (int)C_UnsafeChannelSuccess ||
        wait_done(&recv_state.done) != 0 ||
        atomic_load(&recv_state.code) != (int)C_UnsafeChannelSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    printf("unsafe_channel received value=%d empty=%d\n",
           recv_state.value,
           galay_kernel_unsafe_channel_empty(&channel) ? 1 : 0);

cleanup:
    if (producer_waiter.waiter != 0) {
        (void)galay_kernel_async_waiter_destroy(&producer_waiter);
    }
    if (channel.channel != 0) {
        (void)galay_kernel_unsafe_channel_destroy(&channel);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}
