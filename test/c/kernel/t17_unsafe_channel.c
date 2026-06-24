#include <galay/c/galay-kernel-c/concurrency-c/async_waiter_c.h>
#include <galay/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h>

#include <stdatomic.h>
#include <stddef.h>
#include <time.h>

typedef struct RecvState {
    atomic_int done;
    atomic_int code;
    void* data;
    size_t size;
    void* user;
} RecvState;

typedef struct BatchState {
    atomic_int done;
    atomic_int code;
    size_t count;
    int values[8];
} BatchState;

typedef struct ProducerCtx {
    galay_kernel_unsafe_channel_t* channel;
    const C_UnsafeChannelMessage* messages;
    size_t count;
    atomic_int done;
    atomic_int code;
} ProducerCtx;

static int expect_status(C_UnsafeChannelResultCode actual, C_UnsafeChannelResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void init_recv_state(RecvState* state)
{
    atomic_init(&state->done, 0);
    atomic_init(&state->code, (int)C_UnsafeChannelIOFailed);
    state->data = 0;
    state->size = 0;
    state->user = 0;
}

static void init_batch_state(BatchState* state)
{
    atomic_init(&state->done, 0);
    atomic_init(&state->code, (int)C_UnsafeChannelIOFailed);
    state->count = 0;
    for (size_t i = 0; i < 8; ++i) {
        state->values[i] = 0;
    }
}

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

static int wait_still_pending(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 50; ++i) {
        if (atomic_load(done)) {
            return 1;
        }
        nanosleep(&pause, 0);
    }
    return 0;
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

static int create_started_runtime(galay_kernel_runtime_t* runtime)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    if (galay_kernel_runtime_create(&config, runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(runtime) != C_RuntimeSuccess) {
        (void)galay_kernel_runtime_destroy(runtime);
        return 1;
    }
    return 0;
}

static void on_recv(galay_kernel_unsafe_channel_recv_result_t* result, void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    atomic_store(&state->code, (int)result->code);
    state->data = result->message.data;
    state->size = result->message.size;
    state->user = result->message.user;
    atomic_store(&state->done, 1);
}

static void on_batch(galay_kernel_unsafe_channel_recv_result_t* result, void* ctx)
{
    BatchState* state = (BatchState*)ctx;
    atomic_store(&state->code, (int)result->code);
    state->count = result->count;
    for (size_t i = 0; i < result->count && i < 8; ++i) {
        state->values[i] = *(int*)result->messages[i].data;
    }
    atomic_store(&state->done, 1);
}

static void on_produce(C_AsyncWaiterResultCode code, void* ctx)
{
    ProducerCtx* producer = (ProducerCtx*)ctx;
    C_UnsafeChannelResultCode result = C_UnsafeChannelIOFailed;
    if (code == C_AsyncWaiterSuccess) {
        if (producer->count == 1) {
            result = galay_kernel_unsafe_channel_send(producer->channel, &producer->messages[0]);
        } else {
            result = galay_kernel_unsafe_channel_send_batch(
                producer->channel,
                producer->messages,
                producer->count);
        }
    }
    atomic_store(&producer->code, (int)result);
    atomic_store(&producer->done, 1);
}

static int start_producer(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_waiter_t* waiter,
    ProducerCtx* producer)
{
    atomic_store(&producer->done, 0);
    atomic_store(&producer->code, (int)C_UnsafeChannelIOFailed);
    if (galay_kernel_async_waiter_create(waiter) != C_AsyncWaiterSuccess) {
        return 1;
    }
    if (galay_kernel_async_waiter_wait(runtime, waiter, on_produce, producer) != C_AsyncWaiterSuccess) {
        return 1;
    }
    if (wait_waiter_waiting(waiter) != 0) {
        return 1;
    }
    if (galay_kernel_async_waiter_notify(waiter) != C_AsyncWaiterSuccess) {
        return 1;
    }
    return wait_done(&producer->done);
}

static int test_try_recv(void)
{
    galay_kernel_unsafe_channel_t channel = {0};
    int payload1 = 7;
    int payload2 = 11;
    int payload3 = 13;
    int user1 = 17;
    C_UnsafeChannelMessage input = {&payload1, sizeof(payload1), &user1};
    C_UnsafeChannelMessage out = {0};
    C_UnsafeChannelMessage batch[] = {
        {&payload1, sizeof(payload1), 0},
        {&payload2, sizeof(payload2), 0},
        {&payload3, sizeof(payload3), 0},
    };
    C_UnsafeChannelMessage out_batch[3] = {0};
    size_t count = 99;
    int exit_code = 0;

    if (galay_kernel_unsafe_channel_get_error(C_UnsafeChannelSuccess) == 0) {
        return 1;
    }
    if (expect_status(galay_kernel_unsafe_channel_create(0, C_UnsafeChannelWakeModeInline),
                      C_UnsafeChannelParameterInvalid)) {
        return 2;
    }
    if (expect_status(galay_kernel_unsafe_channel_create(&channel, (C_UnsafeChannelWakeMode)99),
                      C_UnsafeChannelParameterInvalid)) {
        return 3;
    }
    if (expect_status(galay_kernel_unsafe_channel_destroy(0), C_UnsafeChannelParameterInvalid)) {
        return 4;
    }
    if (expect_status(galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline),
                      C_UnsafeChannelSuccess)) {
        return 5;
    }
    if (channel.channel == 0 ||
        !galay_kernel_unsafe_channel_empty(&channel) ||
        galay_kernel_unsafe_channel_size(&channel) != 0) {
        exit_code = 6;
        goto cleanup;
    }
    if (expect_status(galay_kernel_unsafe_channel_try_recv(&channel, &out),
                      C_UnsafeChannelTimeout)) {
        exit_code = 7;
        goto cleanup;
    }
    if (expect_status(galay_kernel_unsafe_channel_send(&channel, &input), C_UnsafeChannelSuccess) ||
        galay_kernel_unsafe_channel_empty(&channel) ||
        galay_kernel_unsafe_channel_size(&channel) != 1) {
        exit_code = 8;
        goto cleanup;
    }
    if (expect_status(galay_kernel_unsafe_channel_try_recv(&channel, &out), C_UnsafeChannelSuccess) ||
        out.data != &payload1 ||
        out.size != sizeof(payload1) ||
        out.user != &user1 ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 9;
        goto cleanup;
    }
    if (expect_status(galay_kernel_unsafe_channel_send_batch(&channel, batch, 3), C_UnsafeChannelSuccess) ||
        galay_kernel_unsafe_channel_size(&channel) != 3) {
        exit_code = 10;
        goto cleanup;
    }
    count = 0;
    if (expect_status(galay_kernel_unsafe_channel_try_recv_batch(&channel, out_batch, 2, &count),
                      C_UnsafeChannelSuccess) ||
        count != 2 ||
        out_batch[0].data != &payload1 ||
        out_batch[1].data != &payload2 ||
        galay_kernel_unsafe_channel_size(&channel) != 1) {
        exit_code = 11;
        goto cleanup;
    }
    count = 0;
    if (expect_status(galay_kernel_unsafe_channel_try_recv_batch(&channel, out_batch, 3, &count),
                      C_UnsafeChannelSuccess) ||
        count != 1 ||
        out_batch[0].data != &payload3 ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 12;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        (void)galay_kernel_unsafe_channel_destroy(&channel);
    }
    return exit_code;
}

static int test_same_runtime_recv_wakeup(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    galay_kernel_async_waiter_t producer_waiter = {0};
    int payload = 101;
    int user = 202;
    C_UnsafeChannelMessage message = {&payload, sizeof(payload), &user};
    RecvState recv_state;
    ProducerCtx producer = {0};
    int exit_code = 0;
    init_recv_state(&recv_state);
    atomic_init(&producer.done, 0);
    atomic_init(&producer.code, (int)C_UnsafeChannelIOFailed);
    producer.channel = &channel;
    producer.messages = &message;
    producer.count = 1;

    if (create_started_runtime(&runtime) != 0 ||
        galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline) != C_UnsafeChannelSuccess) {
        return 20;
    }
    if (expect_status(galay_kernel_unsafe_channel_recv(&runtime, &channel, on_recv, &recv_state),
                      C_UnsafeChannelSuccess)) {
        exit_code = 21;
        goto cleanup;
    }
    if (start_producer(&runtime, &producer_waiter, &producer) != 0 ||
        atomic_load(&producer.code) != (int)C_UnsafeChannelSuccess) {
        exit_code = 22;
        goto cleanup;
    }
    if (wait_done(&recv_state.done) != 0 ||
        atomic_load(&recv_state.code) != (int)C_UnsafeChannelSuccess ||
        recv_state.data != &payload ||
        recv_state.size != sizeof(payload) ||
        recv_state.user != &user ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 23;
        goto cleanup;
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
    return exit_code;
}

static int test_batch_receive(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    int values[] = {1, 2, 3};
    C_UnsafeChannelMessage messages[] = {
        {&values[0], sizeof(values[0]), 0},
        {&values[1], sizeof(values[1]), 0},
        {&values[2], sizeof(values[2]), 0},
    };
    BatchState state;
    int exit_code = 0;
    init_batch_state(&state);

    if (create_started_runtime(&runtime) != 0 ||
        galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeDeferred) != C_UnsafeChannelSuccess) {
        return 30;
    }
    if (expect_status(galay_kernel_unsafe_channel_send_batch(&channel, messages, 3), C_UnsafeChannelSuccess) ||
        expect_status(galay_kernel_unsafe_channel_recv_batch(&runtime, &channel, 8, on_batch, &state),
                      C_UnsafeChannelSuccess)) {
        exit_code = 31;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_UnsafeChannelSuccess ||
        state.count != 3 ||
        state.values[0] != 1 ||
        state.values[1] != 2 ||
        state.values[2] != 3 ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 32;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        (void)galay_kernel_unsafe_channel_destroy(&channel);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}

static int test_batched_threshold(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    galay_kernel_async_waiter_t first_waiter = {0};
    galay_kernel_async_waiter_t second_waiter = {0};
    int values[] = {10, 20, 30};
    C_UnsafeChannelMessage first_messages[] = {
        {&values[0], sizeof(values[0]), 0},
        {&values[1], sizeof(values[1]), 0},
    };
    C_UnsafeChannelMessage second_message = {&values[2], sizeof(values[2]), 0};
    BatchState state;
    ProducerCtx first = {0};
    ProducerCtx second = {0};
    int exit_code = 0;
    init_batch_state(&state);
    atomic_init(&first.done, 0);
    atomic_init(&first.code, (int)C_UnsafeChannelIOFailed);
    atomic_init(&second.done, 0);
    atomic_init(&second.code, (int)C_UnsafeChannelIOFailed);
    first.channel = &channel;
    first.messages = first_messages;
    first.count = 2;
    second.channel = &channel;
    second.messages = &second_message;
    second.count = 1;

    if (create_started_runtime(&runtime) != 0 ||
        galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline) != C_UnsafeChannelSuccess) {
        return 40;
    }
    if (expect_status(galay_kernel_unsafe_channel_recv_batched(&runtime, &channel, 3, on_batch, &state),
                      C_UnsafeChannelSuccess)) {
        exit_code = 41;
        goto cleanup;
    }
    if (start_producer(&runtime, &first_waiter, &first) != 0 ||
        atomic_load(&first.code) != (int)C_UnsafeChannelSuccess ||
        wait_still_pending(&state.done) != 0) {
        exit_code = 42;
        goto cleanup;
    }
    if (start_producer(&runtime, &second_waiter, &second) != 0 ||
        atomic_load(&second.code) != (int)C_UnsafeChannelSuccess) {
        exit_code = 43;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_UnsafeChannelSuccess ||
        state.count != 3 ||
        state.values[0] != 10 ||
        state.values[1] != 20 ||
        state.values[2] != 30 ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 44;
        goto cleanup;
    }

cleanup:
    if (first_waiter.waiter != 0) {
        (void)galay_kernel_async_waiter_destroy(&first_waiter);
    }
    if (second_waiter.waiter != 0) {
        (void)galay_kernel_async_waiter_destroy(&second_waiter);
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

static int test_timeout(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    int partial_value = 77;
    C_UnsafeChannelMessage partial_message = {&partial_value, sizeof(partial_value), 0};
    RecvState recv_state;
    BatchState batch_state;
    BatchState partial_state;
    int exit_code = 0;
    init_recv_state(&recv_state);
    init_batch_state(&batch_state);
    init_batch_state(&partial_state);

    if (create_started_runtime(&runtime) != 0 ||
        galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline) != C_UnsafeChannelSuccess) {
        return 50;
    }
    if (expect_status(galay_kernel_unsafe_channel_recv_timeout(&runtime, &channel, 20, on_recv, &recv_state),
                      C_UnsafeChannelSuccess)) {
        exit_code = 51;
        goto cleanup;
    }
    if (wait_done(&recv_state.done) != 0 ||
        atomic_load(&recv_state.code) != (int)C_UnsafeChannelTimeout) {
        exit_code = 52;
        goto cleanup;
    }
    if (expect_status(galay_kernel_unsafe_channel_recv_batched_timeout(&runtime, &channel, 2, 20, on_batch, &batch_state),
                      C_UnsafeChannelSuccess)) {
        exit_code = 53;
        goto cleanup;
    }
    if (wait_done(&batch_state.done) != 0 ||
        atomic_load(&batch_state.code) != (int)C_UnsafeChannelTimeout ||
        batch_state.count != 0) {
        exit_code = 54;
        goto cleanup;
    }
    if (expect_status(galay_kernel_unsafe_channel_send(&channel, &partial_message), C_UnsafeChannelSuccess) ||
        expect_status(galay_kernel_unsafe_channel_recv_batched_timeout(&runtime, &channel, 2, 20, on_batch, &partial_state),
                      C_UnsafeChannelSuccess)) {
        exit_code = 55;
        goto cleanup;
    }
    if (wait_done(&partial_state.done) != 0 ||
        atomic_load(&partial_state.code) != (int)C_UnsafeChannelSuccess ||
        partial_state.count != 1 ||
        partial_state.values[0] != 77 ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 56;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        (void)galay_kernel_unsafe_channel_destroy(&channel);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}

static int test_stopped_runtime(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    RecvState recv_state;
    BatchState batch_state;
    int exit_code = 0;
    init_recv_state(&recv_state);
    init_batch_state(&batch_state);

    if (create_started_runtime(&runtime) != 0 ||
        galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess ||
        galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline) != C_UnsafeChannelSuccess) {
        return 60;
    }
    if (expect_status(galay_kernel_unsafe_channel_recv(&runtime, &channel, on_recv, &recv_state),
                      C_UnsafeChannelRuntimeNotRunning) ||
        expect_status(galay_kernel_unsafe_channel_recv_timeout(&runtime, &channel, 1, on_recv, &recv_state),
                      C_UnsafeChannelRuntimeNotRunning) ||
        expect_status(galay_kernel_unsafe_channel_recv_batch(&runtime, &channel, 2, on_batch, &batch_state),
                      C_UnsafeChannelRuntimeNotRunning) ||
        expect_status(galay_kernel_unsafe_channel_recv_batched(&runtime, &channel, 2, on_batch, &batch_state),
                      C_UnsafeChannelRuntimeNotRunning) ||
        atomic_load(&recv_state.done) != 0 ||
        atomic_load(&batch_state.done) != 0) {
        exit_code = 61;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        (void)galay_kernel_unsafe_channel_destroy(&channel);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    return exit_code;
}

int main(void)
{
    int result = test_try_recv();
    if (result != 0) {
        return result;
    }
    result = test_same_runtime_recv_wakeup();
    if (result != 0) {
        return result;
    }
    result = test_batch_receive();
    if (result != 0) {
        return result;
    }
    result = test_batched_threshold();
    if (result != 0) {
        return result;
    }
    result = test_timeout();
    if (result != 0) {
        return result;
    }
    return test_stopped_runtime();
}
