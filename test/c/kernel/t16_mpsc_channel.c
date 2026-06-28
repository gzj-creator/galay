#include <galay/c/galay-kernel-c/concurrency-c/mpsc_channel_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stddef.h>
#include <stdint.h>

typedef struct RecvState {
    galay_kernel_mpsc_channel_t* channel;
    C_IOResult result;
    C_MpscChannelMessage message;
    C_MpscChannelMessage messages[4];
    size_t count;
    int64_t timeout_ms;
} RecvState;

static int expect_status(C_MpscChannelResultCode actual, C_MpscChannelResultCode expected)
{
    return actual == expected ? 0 : 1;
}

static int expect_io_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
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
        if (galay_kernel_runtime_destroy(runtime) != C_RuntimeSuccess) {
            return 2;
        }
        return 1;
    }
    return 0;
}

static void recv_entry(void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    state->result =
        galay_kernel_mpsc_channel_recv(state->channel, &state->message, state->timeout_ms);
}

static void recv_batch_entry(void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    state->result =
        galay_kernel_mpsc_channel_recv_batch(state->channel,
                                             state->messages,
                                             4,
                                             &state->count,
                                             state->timeout_ms);
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
    if (expect_status(galay_kernel_mpsc_channel_try_recv(&channel, &message), C_MpscChannelSuccess) ||
        message.data != &first ||
        message.size != sizeof(first) ||
        message.user != &second ||
        !galay_kernel_mpsc_channel_empty(&channel)) {
        exit_code = 5;
        goto cleanup;
    }

cleanup:
    if (channel.channel != 0) {
        if (galay_kernel_mpsc_channel_destroy(&channel) != C_MpscChannelSuccess &&
            exit_code == 0) {
            exit_code = 6;
        }
    }
    return exit_code;
}

static int test_recv_wakeup(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_mpsc_channel_t channel = {0};
    galay_coro_task_t task = {0};
    RecvState state = {0};
    int value = 42;
    int exit_code = 0;

    if (create_started_runtime(&runtime) != 0) {
        return 10;
    }
    if (expect_status(galay_kernel_mpsc_channel_create(&channel), C_MpscChannelSuccess)) {
        exit_code = 11;
        goto cleanup;
    }
    state.channel = &channel;
    state.timeout_ms = 2000;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_entry, &state, 0, &task),
            C_IOResultOk)) {
        exit_code = 12;
        goto cleanup;
    }
    C_MpscChannelMessage sent = {&value, sizeof(value), &state};
    if (expect_status(galay_kernel_mpsc_channel_send(&channel, &sent), C_MpscChannelSuccess) ||
        expect_io_code(galay_coro_join(&task, 3000), C_IOResultOk) ||
        state.result.code != C_IOResultOk ||
        state.message.data != &value ||
        state.message.size != sizeof(value) ||
        state.message.user != &state) {
        exit_code = 13;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 14;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_mpsc_channel_destroy(&channel) != C_MpscChannelSuccess &&
            exit_code == 0) {
            exit_code = 15;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 16;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 17;
        }
    }
    return exit_code;
}

static int test_batch_receive(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_mpsc_channel_t channel = {0};
    galay_coro_task_t task = {0};
    RecvState state = {0};
    int values[3] = {1, 2, 3};
    int exit_code = 0;

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
    state.channel = &channel;
    state.timeout_ms = 2000;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_batch_entry, &state, 0, &task),
            C_IOResultOk) ||
        expect_io_code(galay_coro_join(&task, 3000), C_IOResultOk) ||
        state.result.code != C_IOResultOk ||
        state.count != 3 ||
        state.messages[0].data != &values[0] ||
        state.messages[1].data != &values[1] ||
        state.messages[2].data != &values[2] ||
        !galay_kernel_mpsc_channel_empty(&channel)) {
        exit_code = 23;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 24;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_mpsc_channel_destroy(&channel) != C_MpscChannelSuccess &&
            exit_code == 0) {
            exit_code = 25;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 26;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 27;
        }
    }
    return exit_code;
}

static int test_timeout(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_mpsc_channel_t channel = {0};
    galay_coro_task_t task = {0};
    RecvState state = {0};
    int exit_code = 0;

    if (create_started_runtime(&runtime) != 0) {
        return 30;
    }
    if (expect_status(galay_kernel_mpsc_channel_create(&channel), C_MpscChannelSuccess)) {
        exit_code = 31;
        goto cleanup;
    }
    state.channel = &channel;
    state.timeout_ms = 20;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_entry, &state, 0, &task),
            C_IOResultOk) ||
        expect_io_code(galay_coro_join(&task, 2000), C_IOResultOk) ||
        state.result.code != C_IOResultTimeout ||
        state.message.data != 0) {
        exit_code = 32;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_destroy(&task), C_IOResultOk)) {
        exit_code = 33;
        goto cleanup;
    }

    state.result.code = C_IOResultError;
    state.count = 99;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_batch_entry, &state, 0, &task),
            C_IOResultOk) ||
        expect_io_code(galay_coro_join(&task, 2000), C_IOResultOk) ||
        state.result.code != C_IOResultTimeout ||
        state.count != 0) {
        exit_code = 34;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 35;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_mpsc_channel_destroy(&channel) != C_MpscChannelSuccess &&
            exit_code == 0) {
            exit_code = 36;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 37;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 38;
        }
    }
    return exit_code;
}

int main(void)
{
    galay_kernel_mpsc_channel_t invalid = {0};
    C_MpscChannelMessage message = {0};
    C_MpscChannelMessage messages[2] = {{0}};
    size_t count = 0;
    if (galay_kernel_mpsc_channel_get_error(C_MpscChannelSuccess) == 0 ||
        expect_status(galay_kernel_mpsc_channel_create(0), C_MpscChannelParameterInvalid) ||
        expect_status(galay_kernel_mpsc_channel_destroy(0), C_MpscChannelParameterInvalid) ||
        expect_status(galay_kernel_mpsc_channel_send(0, &message), C_MpscChannelParameterInvalid) ||
        expect_status(galay_kernel_mpsc_channel_try_recv(0, &message), C_MpscChannelParameterInvalid) ||
        expect_io_code(galay_kernel_mpsc_channel_recv(0, &message, -1), C_IOResultInvalid) ||
        expect_io_code(galay_kernel_mpsc_channel_recv(&invalid, &message, -1), C_IOResultInvalid) ||
        expect_io_code(galay_kernel_mpsc_channel_recv_batch(&invalid, messages, 2, &count, -1), C_IOResultInvalid)) {
        return 1;
    }

    int result = test_try_recv();
    if (result != 0) {
        return result;
    }
    result = test_recv_wakeup();
    if (result != 0) {
        return result;
    }
    result = test_batch_receive();
    if (result != 0) {
        return result;
    }
    return test_timeout();
}
