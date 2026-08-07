#include <galay/c/galay-kernel-c/concurrency-c/mpmc/bounded_channel_c.h>

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

enum {
    MAX_PRODUCER_COUNT = 4,
    MAX_CONSUMER_COUNT = 4,
    MESSAGES_PER_SAMPLE = 10000000,
    CHANNEL_CAPACITY = 4096,
    WARMUP_SAMPLES = 1,
    MEASURED_SAMPLES = 5,
    TIMEOUT_US = 10000000
};

typedef struct SharedState {
    galay_kernel_mpmc_bounded_channel_t* channel;
    uintptr_t* payloads;
    int64_t start_us;
    int producer_count;
    atomic_int ready_threads;
    atomic_int producers_done;
    atomic_int failed;
    atomic_bool start;
} SharedState;

typedef struct ProducerArgs {
    SharedState* shared;
    size_t offset;
    size_t count;
    size_t sent;
} ProducerArgs;

typedef struct ConsumerArgs {
    SharedState* shared;
    uint64_t sum;
    size_t received;
} ConsumerArgs;

typedef struct RunResult {
    double messages_per_second;
    uint64_t sum;
    size_t sent;
    size_t received;
    int64_t elapsed_us;
    bool valid;
} RunResult;

static uintptr_t g_payloads[MESSAGES_PER_SAMPLE];

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static int timed_out(const SharedState* shared)
{
    const int64_t now = now_us();
    return now == 0 || now - shared->start_us > TIMEOUT_US;
}

static void wait_for_start(SharedState* shared)
{
    atomic_fetch_add(&shared->ready_threads, 1);
    while (!atomic_load(&shared->start) && !atomic_load(&shared->failed)) {
        sched_yield();
    }
}

static void* producer_main(void* arg)
{
    ProducerArgs* producer = (ProducerArgs*)arg;
    SharedState* shared = producer->shared;
    size_t sent = 0;
    wait_for_start(shared);
    if (atomic_load(&shared->failed)) {
        atomic_fetch_add(&shared->producers_done, 1);
        return 0;
    }

    uint32_t failed_attempts = 0;
    for (size_t i = 0; i < producer->count; ++i) {
        uintptr_t* payload = &shared->payloads[producer->offset + i];
        C_ChannelMessage message = {payload, sizeof(*payload), 0};
        for (;;) {
            C_ChannelResultCode result =
                galay_kernel_mpmc_bounded_channel_try_send(shared->channel, &message);
            if (result == C_ChannelSuccess) {
                ++sent;
                break;
            }
            if (result != C_ChannelFull ||
                atomic_load(&shared->failed) ||
                ((++failed_attempts & 1023U) == 0U && timed_out(shared))) {
                atomic_store(&shared->failed, 1);
                producer->sent = sent;
                atomic_fetch_add(&shared->producers_done, 1);
                return 0;
            }
            sched_yield();
        }
    }
    producer->sent = sent;
    atomic_fetch_add(&shared->producers_done, 1);
    return 0;
}

static void* consumer_main(void* arg)
{
    ConsumerArgs* consumer = (ConsumerArgs*)arg;
    SharedState* shared = consumer->shared;
    uint64_t sum = 0;
    size_t received = 0;
    wait_for_start(shared);
    if (atomic_load(&shared->failed)) {
        return 0;
    }

    uint32_t failed_attempts = 0;
    for (;;) {
        C_ChannelMessage message = {0};
        C_ChannelResultCode result =
            galay_kernel_mpmc_bounded_channel_try_recv(shared->channel, &message);
        if (result == C_ChannelSuccess) {
            ++received;
            sum += (uint64_t)*(uintptr_t*)message.data;
            continue;
        }
        if (result != C_ChannelEmpty ||
            atomic_load(&shared->failed) ||
            ((++failed_attempts & 1023U) == 0U && timed_out(shared))) {
            atomic_store(&shared->failed, 1);
            consumer->received = received;
            consumer->sum = sum;
            return 0;
        }
        if (atomic_load(&shared->producers_done) == shared->producer_count) {
            consumer->received = received;
            consumer->sum = sum;
            return 0;
        }
        sched_yield();
    }
}

static int wait_until_ready(SharedState* shared, int expected_threads)
{
    const int64_t wait_started_us = now_us();
    if (wait_started_us == 0) {
        return 0;
    }
    while (atomic_load(&shared->ready_threads) != expected_threads) {
        const int64_t now = now_us();
        if (atomic_load(&shared->failed) ||
            now == 0 ||
            now - wait_started_us > TIMEOUT_US) {
            return 0;
        }
        sched_yield();
    }
    return 1;
}

static RunResult run_sample(int producer_count, int consumer_count)
{
    galay_kernel_mpmc_bounded_channel_t channel = {0};
    pthread_t producers[MAX_PRODUCER_COUNT];
    pthread_t consumers[MAX_CONSUMER_COUNT];
    ProducerArgs producer_args[MAX_PRODUCER_COUNT] = {{0}};
    ConsumerArgs consumer_args[MAX_CONSUMER_COUNT] = {{0}};
    SharedState shared = {0};
    RunResult run = {0};
    int started_producers = 0;
    int started_consumers = 0;

    shared.channel = &channel;
    shared.payloads = g_payloads;
    shared.producer_count = producer_count;
    atomic_init(&shared.ready_threads, 0);
    atomic_init(&shared.producers_done, 0);
    atomic_init(&shared.failed, 0);
    atomic_init(&shared.start, false);

    if (galay_kernel_mpmc_bounded_channel_create(&channel, CHANNEL_CAPACITY) !=
        C_ChannelSuccess) {
        return run;
    }

    for (int i = 0; i < consumer_count; ++i) {
        consumer_args[i].shared = &shared;
        if (pthread_create(&consumers[i], 0, consumer_main, &consumer_args[i]) != 0) {
            atomic_store(&shared.failed, 1);
            break;
        }
        ++started_consumers;
    }
    const size_t messages_per_producer =
        (size_t)MESSAGES_PER_SAMPLE / (size_t)producer_count;
    for (int i = 0; i < producer_count && !atomic_load(&shared.failed); ++i) {
        producer_args[i].shared = &shared;
        producer_args[i].offset = (size_t)i * messages_per_producer;
        producer_args[i].count = messages_per_producer;
        if (pthread_create(&producers[i], 0, producer_main, &producer_args[i]) != 0) {
            atomic_store(&shared.failed, 1);
            break;
        }
        ++started_producers;
    }

    if (!atomic_load(&shared.failed) &&
        !wait_until_ready(&shared, producer_count + consumer_count)) {
        atomic_store(&shared.failed, 1);
    }
    shared.start_us = now_us();
    if (shared.start_us == 0) {
        atomic_store(&shared.failed, 1);
    }
    atomic_store(&shared.start, true);

    for (int i = 0; i < started_producers; ++i) {
        if (pthread_join(producers[i], 0) != 0) {
            atomic_store(&shared.failed, 1);
        }
    }
    for (int i = 0; i < started_consumers; ++i) {
        if (pthread_join(consumers[i], 0) != 0) {
            atomic_store(&shared.failed, 1);
        }
    }

    const int64_t finished_us = now_us();
    run.elapsed_us = finished_us > 0 ? finished_us - shared.start_us : 0;
    for (int i = 0; i < started_producers; ++i) {
        run.sent += producer_args[i].sent;
    }
    for (int i = 0; i < started_consumers; ++i) {
        run.received += consumer_args[i].received;
        run.sum += consumer_args[i].sum;
    }

    const uint64_t expected_sum =
        ((uint64_t)(MESSAGES_PER_SAMPLE - 1) * (uint64_t)MESSAGES_PER_SAMPLE) / 2;
    run.valid = started_producers == producer_count &&
        started_consumers == consumer_count &&
        !atomic_load(&shared.failed) &&
        run.sent == MESSAGES_PER_SAMPLE &&
        run.received == MESSAGES_PER_SAMPLE &&
        run.sum == expected_sum &&
        run.elapsed_us > 0 &&
        galay_kernel_mpmc_bounded_channel_empty(&channel);
    if (run.valid) {
        run.messages_per_second =
            (double)run.received * 1000000.0 / (double)run.elapsed_us;
    }

    C_ChannelResultCode destroyed =
        galay_kernel_mpmc_bounded_channel_destroy(&channel);
    if (destroyed != C_ChannelSuccess) {
        run.valid = false;
    }
    return run;
}

static int compare_double(const void* lhs, const void* rhs)
{
    const double left = *(const double*)lhs;
    const double right = *(const double*)rhs;
    return left < right ? -1 : left > right ? 1 : 0;
}

static int run_workload(int producer_count, int consumer_count)
{
    for (int sample = 0; sample < WARMUP_SAMPLES; ++sample) {
        RunResult warmup = run_sample(producer_count, consumer_count);
        if (!warmup.valid) {
            return 1;
        }
    }

    double samples[MEASURED_SAMPLES] = {0};
    for (int sample = 0; sample < MEASURED_SAMPLES; ++sample) {
        RunResult run = run_sample(producer_count, consumer_count);
        if (!run.valid) {
            return 2;
        }
        samples[sample] = run.messages_per_second;
    }
    qsort(samples, MEASURED_SAMPLES, sizeof(samples[0]), compare_double);

    if (printf("bounded_channel_throughput producers=%d consumers=%d capacity=%d messages=%d median_msg_s=%.2f min_msg_s=%.2f max_msg_s=%.2f\n",
               producer_count,
               consumer_count,
               CHANNEL_CAPACITY,
               MESSAGES_PER_SAMPLE,
               samples[MEASURED_SAMPLES / 2],
               samples[0],
               samples[MEASURED_SAMPLES - 1]) < 0) {
        return 3;
    }
    return 0;
}

int main(void)
{
    for (size_t i = 0; i < MESSAGES_PER_SAMPLE; ++i) {
        g_payloads[i] = i;
    }

    int result = run_workload(1, 1);
    if (result != 0) {
        return result;
    }
    return run_workload(MAX_PRODUCER_COUNT, MAX_CONSUMER_COUNT);
}
