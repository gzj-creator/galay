#include <galay/c/galay-kernel-c/concurrency-c/mpsc_channel_c.h>

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

typedef struct BatchState {
    atomic_int done;
    atomic_int code;
    C_MpscChannelMessage messages[8];
    size_t count;
} BatchState;

static void on_batch_recv(galay_kernel_mpsc_channel_recv_result_t* result, void* ctx)
{
    BatchState* state = (BatchState*)ctx;
    atomic_store(&state->code, (int)result->code);
    state->count = result->count;
    for (size_t i = 0; i < result->count && i < 8; ++i) {
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

int main(void)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_mpsc_channel_t channel = {0};
    BatchState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_MpscChannelIOFailed);
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
        galay_kernel_mpsc_channel_recv_batch(&runtime, &channel, 8, on_batch_recv, &state) != C_MpscChannelSuccess) {
        exit_code = 1;
        goto cleanup;
    }

    if (wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_MpscChannelSuccess ||
        state.count != 3) {
        exit_code = 2;
        goto cleanup;
    }

    printf("mpsc_channel batch count=%zu values=%s,%s,%s\n",
           state.count,
           (char*)state.messages[0].data,
           (char*)state.messages[1].data,
           (char*)state.messages[2].data);

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
