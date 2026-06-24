#include <galay/c/galay-kernel-c/concurrency-c/mpsc_channel_c.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct CallbackState {
    atomic_int done;
    atomic_int code;
    C_MpscChannelMessage message;
    C_MpscChannelMessage messages[4];
    size_t count;
} CallbackState;

static int expect_status(C_MpscChannelResultCode actual, C_MpscChannelResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static void on_recv(galay_kernel_mpsc_channel_recv_result_t* result, void* ctx)
{
    CallbackState* state = (CallbackState*)ctx;
    atomic_store(&state->code, (int)result->code);
    state->message = result->message;
    state->count = result->count;
    for (size_t i = 0; i < result->count && i < 4; ++i) {
        state->messages[i] = result->messages[i];
    }
    atomic_store(&state->done, 1);
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

static void init_callback_state(CallbackState* state)
{
    atomic_init(&state->done, 0);
    atomic_init(&state->code, (int)C_MpscChannelIOFailed);
    state->message = (C_MpscChannelMessage){0};
    for (size_t i = 0; i < 4; ++i) {
        state->messages[i] = (C_MpscChannelMessage){0};
    }
    state->count = 0;
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

static int test_try_recv(void)
{
    galay_kernel_mpsc_channel_t channel = {0};
    int first = 7;
    int second = 9;
    C_MpscChannelMessage message = {0};
    int exit_code = 0;

    if (expect_status(galay_kernel_mpsc_channel_create(&channel), C_MpscChannelSuccess)) {
        return 1;
    }
    if (!galay_kernel_mpsc_channel_empty(&channel) ||
        galay_kernel_mpsc_channel_size(&channel) != 0) {
        exit_code = 2;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_try_recv(&channel, &message), C_MpscChannelTimeout)) {
        exit_code = 3;
        goto cleanup;
    }

    C_MpscChannelMessage sent = {&first, sizeof(first), &second};
    if (expect_status(galay_kernel_mpsc_channel_send(&channel, &sent), C_MpscChannelSuccess)) {
        exit_code = 4;
        goto cleanup;
    }
    if (galay_kernel_mpsc_channel_empty(&channel) ||
        galay_kernel_mpsc_channel_size(&channel) != 1) {
        exit_code = 5;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_try_recv(&channel, &message), C_MpscChannelSuccess) ||
        message.data != &first ||
        message.size != sizeof(first) ||
        message.user != &second ||
        !galay_kernel_mpsc_channel_empty(&channel)) {
        exit_code = 6;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        (void)galay_kernel_mpsc_channel_destroy(&channel);
    }
    return exit_code;
}

static int test_async_recv_wakeup(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_mpsc_channel_t channel = {0};
    CallbackState state;
    int value = 42;
    int exit_code = 0;
    init_callback_state(&state);

    if (create_started_runtime(&runtime) != 0) {
        return 10;
    }
    if (expect_status(galay_kernel_mpsc_channel_create(&channel), C_MpscChannelSuccess)) {
        exit_code = 11;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_recv(&runtime, &channel, on_recv, &state), C_MpscChannelSuccess)) {
        exit_code = 12;
        goto cleanup;
    }
    C_MpscChannelMessage sent = {&value, sizeof(value), &state};
    if (expect_status(galay_kernel_mpsc_channel_send(&channel, &sent), C_MpscChannelSuccess)) {
        exit_code = 13;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_MpscChannelSuccess ||
        state.message.data != &value ||
        state.message.size != sizeof(value) ||
        state.message.user != &state ||
        state.count != 0) {
        exit_code = 14;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        (void)galay_kernel_mpsc_channel_destroy(&channel);
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
    galay_kernel_mpsc_channel_t channel = {0};
    CallbackState state;
    int values[3] = {1, 2, 3};
    int exit_code = 0;
    init_callback_state(&state);

    if (create_started_runtime(&runtime) != 0) {
        return 20;
    }
    if (expect_status(galay_kernel_mpsc_channel_create(&channel), C_MpscChannelSuccess)) {
        exit_code = 21;
        goto cleanup;
    }

    C_MpscChannelMessage sent[3] = {
        {&values[0], sizeof(values[0]), 0},
        {&values[1], sizeof(values[1]), &values[0]},
        {&values[2], sizeof(values[2]), &values[1]},
    };
    if (expect_status(galay_kernel_mpsc_channel_send_batch(&channel, sent, 3), C_MpscChannelSuccess)) {
        exit_code = 22;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_recv_batch(&runtime, &channel, 4, on_recv, &state), C_MpscChannelSuccess)) {
        exit_code = 23;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_MpscChannelSuccess ||
        state.count != 3 ||
        state.messages[0].data != &values[0] ||
        state.messages[1].data != &values[1] ||
        state.messages[2].data != &values[2] ||
        !galay_kernel_mpsc_channel_empty(&channel)) {
        exit_code = 24;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        (void)galay_kernel_mpsc_channel_destroy(&channel);
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
    galay_kernel_mpsc_channel_t channel = {0};
    CallbackState state;
    int exit_code = 0;
    init_callback_state(&state);

    if (create_started_runtime(&runtime) != 0) {
        return 30;
    }
    if (expect_status(galay_kernel_mpsc_channel_create(&channel), C_MpscChannelSuccess)) {
        exit_code = 31;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_recv_timeout(&runtime, &channel, 20, on_recv, &state), C_MpscChannelSuccess)) {
        exit_code = 32;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_MpscChannelTimeout ||
        state.message.data != 0 ||
        state.count != 0) {
        exit_code = 33;
        goto cleanup;
    }

    init_callback_state(&state);
    if (expect_status(galay_kernel_mpsc_channel_recv_batch_timeout(&runtime, &channel, 2, 20, on_recv, &state), C_MpscChannelSuccess)) {
        exit_code = 34;
        goto cleanup;
    }
    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_MpscChannelTimeout ||
        state.count != 0) {
        exit_code = 35;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        (void)galay_kernel_mpsc_channel_destroy(&channel);
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
    galay_kernel_mpsc_channel_t channel = {0};
    CallbackState state;
    int exit_code = 0;
    init_callback_state(&state);

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 40;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 41;
        goto cleanup;
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess) {
        exit_code = 42;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_create(&channel), C_MpscChannelSuccess)) {
        exit_code = 43;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_recv(&runtime, &channel, on_recv, &state), C_MpscChannelRuntimeNotRunning)) {
        exit_code = 44;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_recv_timeout(&runtime, &channel, 10, on_recv, &state), C_MpscChannelRuntimeNotRunning)) {
        exit_code = 45;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_recv_batch(&runtime, &channel, 2, on_recv, &state), C_MpscChannelRuntimeNotRunning)) {
        exit_code = 46;
        goto cleanup;
    }
    if (expect_status(galay_kernel_mpsc_channel_recv_batch_timeout(&runtime, &channel, 2, 10, on_recv, &state), C_MpscChannelRuntimeNotRunning)) {
        exit_code = 47;
        goto cleanup;
    }
    if (atomic_load(&state.done) != 0) {
        exit_code = 48;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        (void)galay_kernel_mpsc_channel_destroy(&channel);
    }
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
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
    result = test_async_recv_wakeup();
    if (result != 0) {
        return result;
    }
    result = test_batch_receive();
    if (result != 0) {
        return result;
    }
    result = test_timeout();
    if (result != 0) {
        return result;
    }
    return test_stopped_runtime();
}
