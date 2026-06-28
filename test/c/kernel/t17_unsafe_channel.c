#include <galay/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stddef.h>
#include <stdint.h>

typedef struct RecvState {
    galay_kernel_unsafe_channel_t* channel;
    C_IOResult result;
    C_UnsafeChannelMessage message;
    C_UnsafeChannelMessage messages[8];
    size_t count;
    size_t limit;
    int64_t timeout_ms;
} RecvState;

typedef struct ProducerState {
    galay_kernel_unsafe_channel_t* channel;
    const C_UnsafeChannelMessage* messages;
    size_t count;
    C_UnsafeChannelResultCode result;
    int yield_first;
} ProducerState;

static int expect_status(C_UnsafeChannelResultCode actual, C_UnsafeChannelResultCode expected)
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
        galay_kernel_unsafe_channel_recv(state->channel, &state->message, state->timeout_ms);
}

static void recv_batch_entry(void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    state->result =
        galay_kernel_unsafe_channel_recv_batch(state->channel,
                                               state->messages,
                                               8,
                                               &state->count,
                                               state->timeout_ms);
}

static void recv_batched_entry(void* ctx)
{
    RecvState* state = (RecvState*)ctx;
    state->result =
        galay_kernel_unsafe_channel_recv_batched(state->channel,
                                                 state->limit,
                                                 state->messages,
                                                 8,
                                                 &state->count,
                                                 state->timeout_ms);
}

static void producer_entry(void* ctx)
{
    ProducerState* state = (ProducerState*)ctx;
    if (state->yield_first) {
        C_IOResult yielded = galay_coro_yield();
        if (yielded.code != C_IOResultOk) {
            state->result = C_UnsafeChannelOperationInvalid;
            return;
        }
    }
    if (state->count == 1) {
        state->result =
            galay_kernel_unsafe_channel_send(state->channel, &state->messages[0]);
    } else {
        state->result =
            galay_kernel_unsafe_channel_send_batch(state->channel, state->messages, state->count);
    }
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
        if (galay_kernel_unsafe_channel_destroy(&channel) != C_UnsafeChannelSuccess &&
            exit_code == 0) {
            exit_code = 13;
        }
    }
    return exit_code;
}

static int test_recv_wakeup(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    galay_coro_task_t recv_task = {0};
    galay_coro_task_t producer_task = {0};
    RecvState recv_state = {0};
    ProducerState producer = {0};
    int payload = 101;
    int user = 202;
    C_UnsafeChannelMessage message = {&payload, sizeof(payload), &user};
    int exit_code = 0;

    if (create_started_runtime(&runtime) != 0) {
        return 20;
    }
    if (expect_status(galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline),
                      C_UnsafeChannelSuccess)) {
        exit_code = 21;
        goto cleanup;
    }
    recv_state.channel = &channel;
    recv_state.timeout_ms = 2000;
    producer.channel = &channel;
    producer.messages = &message;
    producer.count = 1;
    producer.result = C_UnsafeChannelIOFailed;
    producer.yield_first = 1;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_entry, &recv_state, 0, &recv_task),
                       C_IOResultOk)) {
        exit_code = 22;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_spawn(&runtime, producer_entry, &producer, 0, &producer_task),
                       C_IOResultOk)) {
        exit_code = 23;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_join(&producer_task, 3000), C_IOResultOk)) {
        exit_code = 24;
        goto cleanup;
    }
    if (producer.result != C_UnsafeChannelSuccess) {
        exit_code = 26;
        goto cleanup;
    }
    if (galay_kernel_unsafe_channel_size(&channel) != 1) {
        exit_code = 27;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_join(&recv_task, 3000), C_IOResultOk)) {
        exit_code = 28;
        goto cleanup;
    }
    if (recv_state.result.code != C_IOResultOk) {
        exit_code = 29;
        goto cleanup;
    }
    if (recv_state.message.data != &payload ||
        recv_state.message.size != sizeof(payload) ||
        recv_state.message.user != &user ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 30;
        goto cleanup;
    }

cleanup:
    if (producer_task.task != 0) {
        if (galay_coro_destroy(&producer_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 31;
        }
    }
    if (recv_task.task != 0) {
        if (galay_coro_destroy(&recv_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 32;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_unsafe_channel_destroy(&channel) != C_UnsafeChannelSuccess &&
            exit_code == 0) {
            exit_code = 33;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 34;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 35;
        }
    }
    return exit_code;
}

static int test_batch_receive(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    galay_coro_task_t task = {0};
    RecvState state = {0};
    int values[] = {1, 2, 3};
    C_UnsafeChannelMessage messages[] = {
        {&values[0], sizeof(values[0]), 0},
        {&values[1], sizeof(values[1]), 0},
        {&values[2], sizeof(values[2]), 0},
    };
    int exit_code = 0;

    if (create_started_runtime(&runtime) != 0) {
        return 30;
    }
    if (expect_status(galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeDeferred),
                      C_UnsafeChannelSuccess)) {
        exit_code = 31;
        goto cleanup;
    }
    if (expect_status(galay_kernel_unsafe_channel_send_batch(&channel, messages, 3),
                      C_UnsafeChannelSuccess)) {
        exit_code = 32;
        goto cleanup;
    }
    state.channel = &channel;
    state.timeout_ms = 2000;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_batch_entry, &state, 0, &task),
                       C_IOResultOk) ||
        expect_io_code(galay_coro_join(&task, 3000), C_IOResultOk) ||
        state.result.code != C_IOResultOk ||
        state.count != 3 ||
        *(int*)state.messages[0].data != 1 ||
        *(int*)state.messages[1].data != 2 ||
        *(int*)state.messages[2].data != 3 ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 33;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 34;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_unsafe_channel_destroy(&channel) != C_UnsafeChannelSuccess &&
            exit_code == 0) {
            exit_code = 35;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 36;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 37;
        }
    }
    return exit_code;
}

static int test_batched_threshold(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    galay_coro_task_t recv_task = {0};
    galay_coro_task_t first_task = {0};
    galay_coro_task_t second_task = {0};
    RecvState state = {0};
    ProducerState first = {0};
    ProducerState second = {0};
    int values[] = {10, 20, 30};
    C_UnsafeChannelMessage first_messages[] = {
        {&values[0], sizeof(values[0]), 0},
        {&values[1], sizeof(values[1]), 0},
    };
    C_UnsafeChannelMessage second_message = {&values[2], sizeof(values[2]), 0};
    int exit_code = 0;

    if (create_started_runtime(&runtime) != 0) {
        return 40;
    }
    if (expect_status(galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline),
                      C_UnsafeChannelSuccess)) {
        exit_code = 41;
        goto cleanup;
    }
    state.channel = &channel;
    state.limit = 3;
    state.timeout_ms = 2000;
    first.channel = &channel;
    first.messages = first_messages;
    first.count = 2;
    first.result = C_UnsafeChannelIOFailed;
    first.yield_first = 1;
    second.channel = &channel;
    second.messages = &second_message;
    second.count = 1;
    second.result = C_UnsafeChannelIOFailed;
    second.yield_first = 1;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_batched_entry, &state, 0, &recv_task),
                       C_IOResultOk) ||
        expect_io_code(galay_coro_spawn(&runtime, producer_entry, &first, 0, &first_task),
                       C_IOResultOk) ||
        expect_io_code(galay_coro_join(&first_task, 3000), C_IOResultOk) ||
        first.result != C_UnsafeChannelSuccess ||
        expect_io_code(galay_coro_join(&recv_task, 20), C_IOResultTimeout)) {
        exit_code = 42;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_spawn(&runtime, producer_entry, &second, 0, &second_task),
                       C_IOResultOk) ||
        expect_io_code(galay_coro_join(&second_task, 3000), C_IOResultOk) ||
        expect_io_code(galay_coro_join(&recv_task, 3000), C_IOResultOk) ||
        second.result != C_UnsafeChannelSuccess ||
        state.result.code != C_IOResultOk ||
        state.count != 3 ||
        *(int*)state.messages[0].data != 10 ||
        *(int*)state.messages[1].data != 20 ||
        *(int*)state.messages[2].data != 30 ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 43;
        goto cleanup;
    }

cleanup:
    if (second_task.task != 0) {
        if (galay_coro_destroy(&second_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 44;
        }
    }
    if (first_task.task != 0) {
        if (galay_coro_destroy(&first_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 45;
        }
    }
    if (recv_task.task != 0) {
        if (galay_coro_destroy(&recv_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 46;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_unsafe_channel_destroy(&channel) != C_UnsafeChannelSuccess &&
            exit_code == 0) {
            exit_code = 47;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 48;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 49;
        }
    }
    return exit_code;
}

static int test_timeout(void)
{
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_unsafe_channel_t channel = {0};
    galay_coro_task_t task = {0};
    RecvState state = {0};
    int partial_value = 77;
    C_UnsafeChannelMessage partial_message = {&partial_value, sizeof(partial_value), 0};
    int exit_code = 0;

    if (create_started_runtime(&runtime) != 0) {
        return 50;
    }
    if (expect_status(galay_kernel_unsafe_channel_create(&channel, C_UnsafeChannelWakeModeInline),
                      C_UnsafeChannelSuccess)) {
        exit_code = 51;
        goto cleanup;
    }
    state.channel = &channel;
    state.timeout_ms = 20;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_entry, &state, 0, &task),
                       C_IOResultOk) ||
        expect_io_code(galay_coro_join(&task, 2000), C_IOResultOk) ||
        state.result.code != C_IOResultTimeout ||
        state.message.data != 0) {
        exit_code = 52;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_destroy(&task), C_IOResultOk)) {
        exit_code = 53;
        goto cleanup;
    }

    state.result.code = C_IOResultError;
    state.count = 99;
    state.limit = 2;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_batched_entry, &state, 0, &task),
                       C_IOResultOk) ||
        expect_io_code(galay_coro_join(&task, 2000), C_IOResultOk) ||
        state.result.code != C_IOResultTimeout ||
        state.count != 0) {
        exit_code = 54;
        goto cleanup;
    }
    if (expect_io_code(galay_coro_destroy(&task), C_IOResultOk)) {
        exit_code = 55;
        goto cleanup;
    }

    if (expect_status(galay_kernel_unsafe_channel_send(&channel, &partial_message),
                      C_UnsafeChannelSuccess)) {
        exit_code = 56;
        goto cleanup;
    }
    state.result.code = C_IOResultError;
    state.count = 0;
    if (expect_io_code(galay_coro_spawn(&runtime, recv_batched_entry, &state, 0, &task),
                       C_IOResultOk) ||
        expect_io_code(galay_coro_join(&task, 2000), C_IOResultOk) ||
        state.result.code != C_IOResultOk ||
        state.count != 1 ||
        *(int*)state.messages[0].data != 77 ||
        !galay_kernel_unsafe_channel_empty(&channel)) {
        exit_code = 57;
        goto cleanup;
    }

cleanup:
    if (task.task != 0) {
        if (galay_coro_destroy(&task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 58;
        }
    }
    if (channel.channel != 0) {
        if (galay_kernel_unsafe_channel_destroy(&channel) != C_UnsafeChannelSuccess &&
            exit_code == 0) {
            exit_code = 59;
        }
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 61;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 62;
        }
    }
    return exit_code;
}

static int test_invalid_parameters(void)
{
    galay_kernel_unsafe_channel_t invalid = {0};
    C_UnsafeChannelMessage message = {0};
    C_UnsafeChannelMessage messages[2] = {0};
    size_t count = 0;

    if (expect_status(galay_kernel_unsafe_channel_send(0, &message),
                      C_UnsafeChannelParameterInvalid) ||
        expect_status(galay_kernel_unsafe_channel_send(&invalid, &message),
                      C_UnsafeChannelParameterInvalid) ||
        expect_status(galay_kernel_unsafe_channel_try_recv(0, &message),
                      C_UnsafeChannelParameterInvalid) ||
        expect_status(galay_kernel_unsafe_channel_try_recv_batch(&invalid, messages, 2, &count),
                      C_UnsafeChannelParameterInvalid) ||
        expect_io_code(galay_kernel_unsafe_channel_recv(0, &message, -1),
                       C_IOResultInvalid) ||
        expect_io_code(galay_kernel_unsafe_channel_recv(&invalid, &message, -1),
                       C_IOResultInvalid) ||
        expect_io_code(galay_kernel_unsafe_channel_recv_batch(&invalid, messages, 2, &count, -1),
                       C_IOResultInvalid) ||
        expect_io_code(galay_kernel_unsafe_channel_recv_batched(&invalid, 2, messages, 2, &count, -1),
                       C_IOResultInvalid)) {
        return 60;
    }
    return 0;
}

int main(void)
{
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
    result = test_batched_threshold();
    if (result != 0) {
        return result;
    }
    result = test_timeout();
    if (result != 0) {
        return result;
    }
    return test_invalid_parameters();
}
