#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

enum {
    UNSAFE_CHANNEL_MESSAGES = 100000
};

typedef struct RecvState {
    atomic_int done;
    atomic_int code;
    atomic_size_t count;
    atomic_uint_fast64_t sum;
} RecvState;

typedef struct ProducerCtx {
    galay_kernel_unsafe_channel_t* channel;
    C_UnsafeChannelMessage* messages;
    size_t count;
    atomic_int done;
    atomic_int code;
} ProducerCtx;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 10000; ++i) {
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
    uint64_t sum = 0;
    atomic_store(&state->code, (int)result->code);
    if (result->code == C_UnsafeChannelSuccess) {
        for (size_t i = 0; i < result->count; ++i) {
            sum += *(uint64_t*)result->messages[i].data;
        }
        atomic_store(&state->count, result->count);
        atomic_store(&state->sum, sum);
    }
    atomic_store(&state->done, 1);
}

static void on_produce(C_AsyncWaiterResultCode code, void* ctx)
{
    ProducerCtx* producer = (ProducerCtx*)ctx;
    C_UnsafeChannelResultCode result = C_UnsafeChannelIOFailed;
    if (code == C_AsyncWaiterSuccess) {
        result = galay_kernel_unsafe_channel_send_batch(
            producer->channel,
            producer->messages,
            producer->count);
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
    uint64_t* payloads = 0;
    C_UnsafeChannelMessage* messages = 0;
    int exit_code = 0;

    RecvState recv_state;
    atomic_init(&recv_state.done, 0);
    atomic_init(&recv_state.code, (int)C_UnsafeChannelIOFailed);
    atomic_init(&recv_state.count, 0);
    atomic_init(&recv_state.sum, 0);

    ProducerCtx producer;
    producer.channel = &channel;
    producer.messages = 0;
    producer.count = UNSAFE_CHANNEL_MESSAGES;
    atomic_init(&producer.done, 0);
    atomic_init(&producer.code, (int)C_UnsafeChannelIOFailed);

    payloads = (uint64_t*)malloc(sizeof(uint64_t) * UNSAFE_CHANNEL_MESSAGES);
    messages = (C_UnsafeChannelMessage*)malloc(sizeof(C_UnsafeChannelMessage) * UNSAFE_CHANNEL_MESSAGES);
    if (payloads == 0 || messages == 0) {
        exit_code = 1;
        goto cleanup;
    }
    for (size_t i = 0; i < UNSAFE_CHANNEL_MESSAGES; ++i) {
        payloads[i] = (uint64_t)i;
        messages[i].data = &payloads[i];
        messages[i].size = sizeof(payloads[i]);
        messages[i].user = 0;
    }
    producer.messages = messages;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline) != C_UnsafeChannelSuccess ||
        galay_kernel_unsafe_channel_recv_batched(
            &runtime,
            &channel,
            UNSAFE_CHANNEL_MESSAGES,
            on_recv,
            &recv_state) != C_UnsafeChannelSuccess ||
        galay_kernel_async_waiter_create(&producer_waiter) != C_AsyncWaiterSuccess ||
        galay_kernel_async_waiter_wait(&runtime, &producer_waiter, on_produce, &producer) != C_AsyncWaiterSuccess ||
        wait_waiter_waiting(&producer_waiter) != 0) {
        exit_code = 2;
        goto cleanup;
    }

    const int64_t start = now_us();
    if (galay_kernel_async_waiter_notify(&producer_waiter) != C_AsyncWaiterSuccess ||
        wait_done(&producer.done) != 0 ||
        wait_done(&recv_state.done) != 0) {
        exit_code = 3;
        goto cleanup;
    }
    const int64_t elapsed = now_us() - start;

    const uint64_t expected_sum =
        ((uint64_t)(UNSAFE_CHANNEL_MESSAGES - 1) * (uint64_t)UNSAFE_CHANNEL_MESSAGES) / 2;
    if (atomic_load(&producer.code) != (int)C_UnsafeChannelSuccess ||
        atomic_load(&recv_state.code) != (int)C_UnsafeChannelSuccess ||
        atomic_load(&recv_state.count) != UNSAFE_CHANNEL_MESSAGES ||
        atomic_load(&recv_state.sum) != expected_sum ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 4;
        goto cleanup;
    }

    {
        const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
        const double ops_per_sec = seconds > 0.0
            ? (double)(UNSAFE_CHANNEL_MESSAGES * 2) / seconds
            : 0.0;
        printf("unsafe_channel_throughput scheduler_count=1 messages=%d elapsed_ms=%.3f ops_per_sec=%.2f\n",
               UNSAFE_CHANNEL_MESSAGES,
               (double)elapsed / 1000.0,
               ops_per_sec);
    }

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
    free(messages);
    free(payloads);
    return exit_code;
}
