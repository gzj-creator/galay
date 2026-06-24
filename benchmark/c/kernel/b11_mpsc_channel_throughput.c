#include <galay/c/galay-kernel-c/concurrency-c/mpsc_channel_c.h>

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

enum {
    MPSC_PRODUCER_COUNT = 4,
    MPSC_MESSAGES_PER_PRODUCER = 25000,
    MPSC_TOTAL_MESSAGES = MPSC_PRODUCER_COUNT * MPSC_MESSAGES_PER_PRODUCER,
    MPSC_TIMEOUT_US = 10000000
};

typedef struct ProducerArgs {
    galay_kernel_mpsc_channel_t* channel;
    uintptr_t* payloads;
    size_t offset;
    size_t count;
    atomic_int* done_producers;
    atomic_int* failed;
    atomic_size_t* sent_count;
} ProducerArgs;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void* producer_main(void* arg)
{
    ProducerArgs* args = (ProducerArgs*)arg;
    for (size_t i = 0; i < args->count; ++i) {
        uintptr_t* payload = &args->payloads[args->offset + i];
        C_MpscChannelMessage message = {
            payload,
            sizeof(*payload),
            (void*)(uintptr_t)args->offset
        };
        if (galay_kernel_mpsc_channel_send(args->channel, &message) != C_MpscChannelSuccess) {
            atomic_store(args->failed, 1);
            break;
        }
        atomic_fetch_add(args->sent_count, 1);
    }
    atomic_fetch_add(args->done_producers, 1);
    return 0;
}

int main(void)
{
    galay_kernel_mpsc_channel_t channel = {0};
    pthread_t producers[MPSC_PRODUCER_COUNT];
    ProducerArgs args[MPSC_PRODUCER_COUNT];
    uintptr_t payloads[MPSC_TOTAL_MESSAGES];
    atomic_int done_producers;
    atomic_int failed;
    atomic_size_t sent_count;
    size_t received_count = 0;
    int started_threads = 0;
    int exit_code = 0;

    atomic_init(&done_producers, 0);
    atomic_init(&failed, 0);
    atomic_init(&sent_count, 0);
    for (size_t i = 0; i < MPSC_TOTAL_MESSAGES; ++i) {
        payloads[i] = i;
    }

    if (galay_kernel_mpsc_channel_create(&channel) != C_MpscChannelSuccess) {
        return 1;
    }

    const int64_t start = now_us();
    for (int i = 0; i < MPSC_PRODUCER_COUNT; ++i) {
        args[i].channel = &channel;
        args[i].payloads = payloads;
        args[i].offset = (size_t)i * MPSC_MESSAGES_PER_PRODUCER;
        args[i].count = MPSC_MESSAGES_PER_PRODUCER;
        args[i].done_producers = &done_producers;
        args[i].failed = &failed;
        args[i].sent_count = &sent_count;
        if (pthread_create(&producers[i], 0, producer_main, &args[i]) != 0) {
            atomic_store(&failed, 1);
            exit_code = 2;
            break;
        }
        ++started_threads;
    }

    while (1) {
        if (now_us() - start > MPSC_TIMEOUT_US) {
            exit_code = 5;
            atomic_store(&failed, 1);
            break;
        }
        C_MpscChannelMessage message = {0};
        C_MpscChannelResultCode code = galay_kernel_mpsc_channel_try_recv(&channel, &message);
        if (code == C_MpscChannelSuccess) {
            ++received_count;
            continue;
        }
        if (code != C_MpscChannelTimeout) {
            exit_code = 3;
            atomic_store(&failed, 1);
            break;
        }
        if (atomic_load(&done_producers) == started_threads &&
            received_count == atomic_load(&sent_count)) {
            break;
        }
        sched_yield();
    }

    for (int i = 0; i < started_threads; ++i) {
        (void)pthread_join(producers[i], 0);
    }

    const int64_t elapsed = now_us() - start;
    const size_t sent = atomic_load(&sent_count);
    if (exit_code == 0 && (atomic_load(&failed) != 0 || sent != MPSC_TOTAL_MESSAGES ||
                           received_count != MPSC_TOTAL_MESSAGES)) {
        exit_code = 4;
    }

    if (exit_code == 0) {
        const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
        const double ops_per_sec = seconds > 0.0 ? (double)(sent + received_count) / seconds : 0.0;
        printf("mpsc_channel_throughput producers=%d sent=%zu received=%zu elapsed_ms=%.3f ops_per_sec=%.2f\n",
               MPSC_PRODUCER_COUNT,
               sent,
               received_count,
               (double)elapsed / 1000.0,
               ops_per_sec);
    }

    if (channel.channel != 0) {
        (void)galay_kernel_mpsc_channel_destroy(&channel);
    }
    return exit_code;
}
