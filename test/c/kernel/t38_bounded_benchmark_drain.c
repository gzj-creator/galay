#include <galay/c/galay-kernel-c/concurrency-c/mpmc/bounded_channel.h>

static C_IOResult recv_with_final_publish(galay_c_mpmc_bounded_channel_t* channel,
                                        galay_c_channel_message_t* message);

#define galay_c_mpmc_bounded_channel_try_recv recv_with_final_publish
#define main benchmark_main
#include "../../../benchmark/c/kernel/b27_bounded_channel_throughput.c"
#undef main
#undef galay_c_mpmc_bounded_channel_try_recv

static SharedState* g_shared;
static uintptr_t g_value = 42;
static int g_published;

static C_IOResult recv_with_final_publish(galay_c_mpmc_bounded_channel_t* channel,
                                        galay_c_channel_message_t* message)
{
    const C_IOResult result = galay_c_mpmc_bounded_channel_try_recv(channel, message);
    if (!g_published && result.code == C_IOResultInvalid) {
        const galay_c_channel_message_t final = {&g_value, sizeof(g_value), NULL};
        const C_IOResult sent = galay_c_mpmc_bounded_channel_try_send(channel, &final);
        if (sent.code != C_IOResultOk) {
            atomic_store(&g_shared->failed, 1);
            return sent;
        }
        g_published = 1;
        atomic_store(&g_shared->producers_done, 1);
    }
    return result;
}

int main(void)
{
    galay_c_mpmc_bounded_channel_t channel = {0};
    if (galay_c_mpmc_bounded_channel_create(&channel, 2).code != C_IOResultOk) {
        return 1;
    }
    SharedState shared = {0};
    shared.channel = &channel;
    shared.producer_count = 1;
    shared.start_us = now_us();
    atomic_init(&shared.ready_threads, 0);
    atomic_init(&shared.producers_done, 0);
    atomic_init(&shared.failed, 0);
    atomic_init(&shared.start, true);
    g_shared = &shared;
    ConsumerArgs consumer = {.shared = &shared};
    // Publish the final message after an empty read but before completion is observed.
    if (consumer_main(&consumer) != NULL) {
        return 2;
    }
    const int valid = consumer.received == 1 && consumer.sum == g_value &&
        !atomic_load(&shared.failed) && galay_c_mpmc_bounded_channel_is_empty(&channel);
    if (galay_c_mpmc_bounded_channel_destroy(&channel).code != C_IOResultOk) {
        return 3;
    }
    if (!valid) {
        fprintf(stderr, "final message was not drained: received=%zu sum=%llu\n",
                consumer.received, (unsigned long long)consumer.sum);
        return 4;
    }
    return 0;
}
