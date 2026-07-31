#include <galay/c/galay-kernel-c/concurrency-c/bounded_channel_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stddef.h>
#include <stdint.h>

typedef struct RecvState {
    galay_kernel_bounded_channel_t* channel;
    C_IOResult result;
    C_BoundedChannelMessage message;
    C_BoundedChannelMessage messages[4];
    size_t count;
    int64_t timeout_ms;
} RecvState;

typedef struct SendState {
    galay_kernel_bounded_channel_t* channel;
    C_IOResult result;
    C_BoundedChannelMessage message;
    int64_t timeout_ms;
} SendState;

static int expect_status(C_BoundedChannelResultCode actual,
                         C_BoundedChannelResultCode expected)
{
    return actual == expected ? 0 : 1;
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
    state->result = galay_kernel_bounded_channel_recv(
        state->channel, &state->message, state->timeout_ms);
}

static void recv_batch_entry(void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    state->result = galay_kernel_bounded_channel_recv_batch(
        state->channel,
        state->messages,
        4,
        &state->count,
        state->timeout_ms);
}

static void send_entry(void* ctx)
{
    SendState* state = (SendState*)ctx;
    state->result = galay_kernel_bounded_channel_send(
        state->channel, &state->message, state->timeout_ms);
}

static int test_sync_boundaries(void)
{
    galay_kernel_bounded_channel_t channel = {0};
    galay_kernel_bounded_channel_t minimum_channel = {0};
    int values[] = {11, 22, 33, 44};
    C_BoundedChannelMessage inputs[] = {
        {&values[0], sizeof(values[0]), &values[3]},
        {&values[1], sizeof(values[1]), 0},
        {&values[2], sizeof(values[2]), 0},
        {&values[3], sizeof(values[3]), 0},
    };
    C_BoundedChannelMessage output = {0};
    C_BoundedChannelMessage outputs[3] = {{0}};
    C_BoundedChannelMessage invalid_message = {0, 1, 0};
    size_t count = 99;
    int exit_code = 0;

    if (galay_kernel_bounded_channel_get_error(C_BoundedChannelSuccess) == 0 ||
        galay_kernel_bounded_channel_get_error(C_BoundedChannelParameterInvalid) == 0 ||
        galay_kernel_bounded_channel_get_error(C_BoundedChannelMemoryAllocFailed) == 0 ||
        galay_kernel_bounded_channel_get_error(C_BoundedChannelClosed) == 0 ||
        galay_kernel_bounded_channel_get_error(C_BoundedChannelFull) == 0 ||
        galay_kernel_bounded_channel_get_error(C_BoundedChannelEmpty) == 0 ||
        galay_kernel_bounded_channel_get_error((C_BoundedChannelResultCode)99) == 0) {
        return 1;
    }
    if (expect_status(galay_kernel_bounded_channel_create(0, 2),
                      C_BoundedChannelParameterInvalid) ||
        expect_status(galay_kernel_bounded_channel_destroy(0),
                      C_BoundedChannelParameterInvalid)) {
        return 2;
    }
    if (expect_status(galay_kernel_bounded_channel_create(&minimum_channel, 0),
                      C_BoundedChannelSuccess)) {
        return 2;
    }
    if (galay_kernel_bounded_channel_capacity(&minimum_channel) != 2) {
        if (galay_kernel_bounded_channel_destroy(&minimum_channel) !=
            C_BoundedChannelSuccess) {
            return 2;
        }
        return 2;
    }
    if (expect_status(galay_kernel_bounded_channel_destroy(&minimum_channel),
                      C_BoundedChannelSuccess) ||
        expect_status(galay_kernel_bounded_channel_create(&channel, 3),
                      C_BoundedChannelSuccess)) {
        return 2;
    }
    if (channel.channel == 0 ||
        galay_kernel_bounded_channel_capacity(&channel) != 4 ||
        galay_kernel_bounded_channel_size(&channel) != 0 ||
        !galay_kernel_bounded_channel_empty(&channel) ||
        galay_kernel_bounded_channel_full(&channel) ||
        galay_kernel_bounded_channel_is_closed(&channel)) {
        exit_code = 3;
        goto cleanup;
    }
    if (expect_status(galay_kernel_bounded_channel_try_send(&channel, &invalid_message),
                      C_BoundedChannelParameterInvalid) ||
        expect_status(galay_kernel_bounded_channel_try_recv(&channel, &output),
                      C_BoundedChannelEmpty) ||
        output.data != 0) {
        exit_code = 4;
        goto cleanup;
    }
    if (expect_status(galay_kernel_bounded_channel_try_send(&channel, &inputs[0]),
                      C_BoundedChannelSuccess) ||
        expect_status(galay_kernel_bounded_channel_try_send(&channel, &inputs[1]),
                      C_BoundedChannelSuccess) ||
        expect_status(galay_kernel_bounded_channel_try_send(&channel, &inputs[2]),
                      C_BoundedChannelSuccess)) {
        exit_code = 5;
        goto cleanup;
    }
    count = 0;
    if (expect_status(galay_kernel_bounded_channel_try_recv_batch(
                          &channel, outputs, 2, &count),
                      C_BoundedChannelSuccess) ||
        count != 2 ||
        outputs[0].data != &values[0] ||
        outputs[0].user != &values[3] ||
        outputs[1].data != &values[1] ||
        galay_kernel_bounded_channel_size(&channel) != 1) {
        exit_code = 6;
        goto cleanup;
    }
    count = 0;
    if (expect_status(galay_kernel_bounded_channel_try_send_batch(
                          &channel, inputs, 4, &count),
                      C_BoundedChannelFull) ||
        count != 3 ||
        !galay_kernel_bounded_channel_full(&channel) ||
        galay_kernel_bounded_channel_size(&channel) != 4) {
        exit_code = 7;
        goto cleanup;
    }
    if (expect_status(galay_kernel_bounded_channel_close(&channel),
                      C_BoundedChannelSuccess) ||
        expect_status(galay_kernel_bounded_channel_close(&channel),
                      C_BoundedChannelSuccess) ||
        !galay_kernel_bounded_channel_is_closed(&channel) ||
        expect_status(galay_kernel_bounded_channel_try_send(&channel, &inputs[0]),
                      C_BoundedChannelClosed)) {
        exit_code = 8;
        goto cleanup;
    }
    while (galay_kernel_bounded_channel_try_recv(&channel, &output) ==
           C_BoundedChannelSuccess) {
    }
    if (expect_status(galay_kernel_bounded_channel_try_recv(&channel, &output),
                      C_BoundedChannelClosed) ||
        !galay_kernel_bounded_channel_empty(&channel)) {
        exit_code = 9;
    }

cleanup:
    if (channel.channel != 0) {
        C_BoundedChannelResultCode destroyed =
            galay_kernel_bounded_channel_destroy(&channel);
        if (destroyed != C_BoundedChannelSuccess && exit_code == 0) {
            exit_code = 10;
        }
    }
    return exit_code;
}

static int test_coro_send_recv(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_bounded_channel_t channel = {0};
    galay_coro_task_t task = {0};
    RecvState recv_state = {0};
    SendState send_state = {0};
    int values[] = {101, 202, 303};
    C_BoundedChannelMessage first = {&values[0], sizeof(values[0]), 0};
    C_BoundedChannelMessage second = {&values[1], sizeof(values[1]), 0};
    C_BoundedChannelMessage output = {0};
    int exit_code = 0;

    if (create_started_runtime(&runtime) != 0) {
        return 20;
    }
    if (expect_status(galay_kernel_bounded_channel_create(&channel, 2),
                      C_BoundedChannelSuccess)) {
        exit_code = 21;
        goto cleanup;
    }

    recv_state.channel = &channel;
    recv_state.timeout_ms = 2000;
    if (galay_coro_spawn(&runtime, recv_entry, &recv_state, 0, &task).code != C_IOResultOk ||
        galay_kernel_bounded_channel_try_send(&channel, &first) != C_BoundedChannelSuccess ||
        galay_coro_join(&task, 3000).code != C_IOResultOk ||
        recv_state.result.code != C_IOResultOk ||
        recv_state.result.bytes != 1 ||
        recv_state.message.data != &values[0]) {
        exit_code = 22;
        goto cleanup;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk) {
        exit_code = 23;
        goto cleanup;
    }

    if (galay_kernel_bounded_channel_try_send(&channel, &first) != C_BoundedChannelSuccess ||
        galay_kernel_bounded_channel_try_send(&channel, &second) != C_BoundedChannelSuccess) {
        exit_code = 24;
        goto cleanup;
    }
    send_state.channel = &channel;
    send_state.message.data = &values[2];
    send_state.message.size = sizeof(values[2]);
    send_state.timeout_ms = 2000;
    if (galay_coro_spawn(&runtime, send_entry, &send_state, 0, &task).code != C_IOResultOk ||
        galay_kernel_bounded_channel_try_recv(&channel, &output) != C_BoundedChannelSuccess ||
        galay_coro_join(&task, 3000).code != C_IOResultOk ||
        send_state.result.code != C_IOResultOk ||
        send_state.result.bytes != 1) {
        exit_code = 25;
        goto cleanup;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk) {
        exit_code = 26;
        goto cleanup;
    }

    while (galay_kernel_bounded_channel_try_recv(&channel, &output) ==
           C_BoundedChannelSuccess) {
    }
    recv_state.count = 0;
    recv_state.timeout_ms = 2000;
    if (galay_kernel_bounded_channel_try_send(&channel, &first) != C_BoundedChannelSuccess ||
        galay_kernel_bounded_channel_try_send(&channel, &second) != C_BoundedChannelSuccess ||
        galay_coro_spawn(&runtime, recv_batch_entry, &recv_state, 0, &task).code != C_IOResultOk ||
        galay_coro_join(&task, 3000).code != C_IOResultOk ||
        recv_state.result.code != C_IOResultOk ||
        recv_state.result.bytes != 2 ||
        recv_state.count != 2 ||
        recv_state.messages[0].data != &values[0] ||
        recv_state.messages[1].data != &values[1]) {
        exit_code = 27;
        goto cleanup;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk) {
        exit_code = 28;
        goto cleanup;
    }

    recv_state.result.code = C_IOResultError;
    recv_state.timeout_ms = 20;
    if (galay_coro_spawn(&runtime, recv_entry, &recv_state, 0, &task).code != C_IOResultOk ||
        galay_coro_join(&task, 2000).code != C_IOResultOk ||
        recv_state.result.code != C_IOResultTimeout) {
        exit_code = 29;
        goto cleanup;
    }
    if (galay_coro_destroy(&task).code != C_IOResultOk) {
        exit_code = 30;
        goto cleanup;
    }

    recv_state.result.code = C_IOResultError;
    recv_state.timeout_ms = -1;
    if (galay_coro_spawn(&runtime, recv_entry, &recv_state, 0, &task).code != C_IOResultOk ||
        galay_kernel_bounded_channel_close(&channel) != C_BoundedChannelSuccess ||
        galay_coro_join(&task, 2000).code != C_IOResultOk ||
        recv_state.result.code != C_IOResultCancelled) {
        exit_code = 31;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        C_IOResult destroyed = galay_coro_destroy(&task);
        if (destroyed.code != C_IOResultOk && exit_code == 0) {
            exit_code = 32;
        }
    }
    if (channel.channel != 0) {
        C_BoundedChannelResultCode destroyed =
            galay_kernel_bounded_channel_destroy(&channel);
        if (destroyed != C_BoundedChannelSuccess && exit_code == 0) {
            exit_code = 33;
        }
    }
    if (runtime.runtime != 0) {
        C_RuntimeResultCode stopped = galay_kernel_runtime_stop(&runtime);
        if (stopped != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 34;
        }
        C_RuntimeResultCode destroyed = galay_kernel_runtime_destroy(&runtime);
        if (destroyed != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 35;
        }
    }
    return exit_code;
}

static int test_batch_and_zero_timeout(void)
{
    galay_kernel_bounded_channel_t channel = {0};
    int value = 7;
    C_BoundedChannelMessage input = {&value, sizeof(value), 0};
    C_BoundedChannelMessage output = {0};
    C_BoundedChannelMessage outputs[2] = {{0}};
    size_t count = 99;
    int exit_code = 0;

    if (galay_kernel_bounded_channel_create(&channel, 2) != C_BoundedChannelSuccess) {
        return 40;
    }
    if (galay_kernel_bounded_channel_recv(&channel, &output, 0).code != C_IOResultTimeout ||
        galay_kernel_bounded_channel_recv(&channel, &output, 1).code != C_IOResultInvalid ||
        galay_kernel_bounded_channel_recv_batch(&channel, outputs, 2, &count, 0).code !=
            C_IOResultTimeout ||
        count != 0 ||
        galay_kernel_bounded_channel_try_send(&channel, &input) != C_BoundedChannelSuccess ||
        galay_kernel_bounded_channel_try_send(&channel, &input) != C_BoundedChannelSuccess ||
        galay_kernel_bounded_channel_send(&channel, &input, 0).code != C_IOResultTimeout ||
        galay_kernel_bounded_channel_send(&channel, &input, 1).code != C_IOResultInvalid) {
        exit_code = 41;
    }
    if (channel.channel != 0) {
        C_BoundedChannelResultCode destroyed =
            galay_kernel_bounded_channel_destroy(&channel);
        if (destroyed != C_BoundedChannelSuccess && exit_code == 0) {
            exit_code = 42;
        }
    }
    return exit_code;
}

int main(void)
{
    int result = test_sync_boundaries();
    if (result != 0) {
        return result;
    }
    result = test_coro_send_recv();
    if (result != 0) {
        return result;
    }
    return test_batch_and_zero_timeout();
}
