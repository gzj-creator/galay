#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#if defined(__linux__)
#include <unistd.h>
#endif

#ifndef CHANNEL_USE_PRODUCER_TOKEN
#define CHANNEL_USE_PRODUCER_TOKEN 0
#endif

#ifndef CHANNEL_USE_CONSUMER_TOKEN
#define CHANNEL_USE_CONSUMER_TOKEN 0
#endif

enum {
    MAX_PRODUCERS = 4,
    MAX_CONSUMERS = 4,
    MESSAGES_PER_SAMPLE = 2000000,
    WARMUP_SAMPLES = 1,
    MEASURED_SAMPLES = 3,
    TIMEOUT_US = 10000000
};

typedef struct SharedState {
    CHANNEL_HANDLE* channel;
    uintptr_t* payloads;
    int64_t start_us;
    atomic_int ready_threads;
    atomic_bool producers_finished;
    atomic_int failed;
    atomic_bool start;
} SharedState;

typedef struct ProducerArgs {
    SharedState* shared;
    size_t core_index;
    size_t offset;
    size_t count;
    size_t sent;
#if CHANNEL_USE_PRODUCER_TOKEN
    CHANNEL_PRODUCER_TOKEN_TYPE token;
#endif
} ProducerArgs;

typedef struct ConsumerArgs {
    SharedState* shared;
    size_t core_index;
    size_t received;
    uint64_t sum;
#if CHANNEL_USE_CONSUMER_TOKEN
    CHANNEL_CONSUMER_TOKEN_TYPE token;
#endif
} ConsumerArgs;

typedef struct RunResult {
    double messages_per_second;
    size_t sent;
    size_t received;
    uint64_t sum;
    int64_t elapsed_us;
    int valid;
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

static int pin_current_thread(size_t core_index)
{
#if defined(__linux__)
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online <= 0) {
        return 0;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((int)(core_index % (size_t)online), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core_index;
    return 1;
#endif
}

static int wait_for_start(SharedState* shared)
{
    atomic_fetch_add(&shared->ready_threads, 1);
    while (!atomic_load(&shared->start) && !atomic_load(&shared->failed)) {
        sched_yield();
    }
    return atomic_load(&shared->failed) == 0;
}

static void* producer_main(void* arg)
{
    ProducerArgs* producer = (ProducerArgs*)arg;
    SharedState* shared = producer->shared;
    size_t sent = 0;

    if (!pin_current_thread(producer->core_index)) {
        atomic_store(&shared->failed, 1);
        return 0;
    }
#if CHANNEL_USE_PRODUCER_TOKEN
    if (CHANNEL_PRODUCER_TOKEN_CREATE(shared->channel, &producer->token) !=
        C_ChannelSuccess) {
        atomic_store(&shared->failed, 1);
        return 0;
    }
#endif
    if (!wait_for_start(shared)) {
        return 0;
    }

    for (size_t index = 0; index < producer->count; ++index) {
        uintptr_t* payload = &shared->payloads[producer->offset + index];
        C_ChannelMessage message = {payload, sizeof(*payload), 0};
        uint32_t retries = 0;
        for (;;) {
            const C_ChannelResultCode result =
#if CHANNEL_USE_PRODUCER_TOKEN
                CHANNEL_TRY_SEND_WITH_PRODUCER_TOKEN(
                    shared->channel, &producer->token, &message);
#else
                CHANNEL_TRY_SEND(shared->channel, &message);
#endif
            if (result == C_ChannelSuccess) {
                ++sent;
                break;
            }
            if (result != CHANNEL_RETRYABLE_SEND_RESULT ||
                atomic_load(&shared->failed) ||
                ((++retries & 1023U) == 0U && timed_out(shared))) {
                atomic_store(&shared->failed, 1);
                producer->sent = sent;
                return 0;
            }
            sched_yield();
        }
    }

    producer->sent = sent;
    return 0;
}

static void* consumer_main(void* arg)
{
    ConsumerArgs* consumer = (ConsumerArgs*)arg;
    SharedState* shared = consumer->shared;
    size_t received = 0;
    uint64_t sum = 0;

    if (!pin_current_thread(consumer->core_index)) {
        atomic_store(&shared->failed, 1);
        return 0;
    }
#if CHANNEL_USE_CONSUMER_TOKEN
    if (CHANNEL_CONSUMER_TOKEN_CREATE(shared->channel, &consumer->token) !=
        C_ChannelSuccess) {
        atomic_store(&shared->failed, 1);
        return 0;
    }
#endif
    if (!wait_for_start(shared)) {
        return 0;
    }

    for (;;) {
        C_ChannelMessage message = {0};
        const C_ChannelResultCode result =
#if CHANNEL_USE_CONSUMER_TOKEN
            CHANNEL_TRY_RECV_WITH_CONSUMER_TOKEN(
                shared->channel, &consumer->token, &message);
#else
            CHANNEL_TRY_RECV(shared->channel, &message);
#endif
        if (result == C_ChannelSuccess) {
            if (message.data == 0 || message.size != sizeof(uintptr_t)) {
                atomic_store(&shared->failed, 1);
                break;
            }
            ++received;
            sum += *(uintptr_t*)message.data;
            continue;
        }
        if (CHANNEL_HAS_CLOSE && result == C_ChannelClosed) {
            break;
        }
        if (!CHANNEL_HAS_CLOSE &&
            atomic_load(&shared->producers_finished) &&
            received == MESSAGES_PER_SAMPLE) {
            break;
        }
        if (result != C_ChannelEmpty || atomic_load(&shared->failed) ||
            timed_out(shared)) {
            atomic_store(&shared->failed, 1);
            break;
        }
        sched_yield();
    }

    consumer->received = received;
    consumer->sum = sum;
    return 0;
}

static int wait_until_ready(SharedState* shared, int expected_threads)
{
    const int64_t wait_started_us = now_us();
    if (wait_started_us == 0) {
        return 0;
    }
    while (atomic_load(&shared->ready_threads) != expected_threads) {
        const int64_t now = now_us();
        if (atomic_load(&shared->failed) || now == 0 ||
            now - wait_started_us > TIMEOUT_US) {
            return 0;
        }
        sched_yield();
    }
    return 1;
}

static RunResult run_sample(void)
{
    CHANNEL_HANDLE channel = {0};
    pthread_t producers[MAX_PRODUCERS];
    pthread_t consumers[MAX_CONSUMERS];
    ProducerArgs producer_args[MAX_PRODUCERS] = {{0}};
    ConsumerArgs consumer_args[MAX_CONSUMERS] = {{0}};
    SharedState shared = {0};
    RunResult run = {0};
    int started_producers = 0;
    int started_consumers = 0;

    shared.channel = &channel;
    shared.payloads = g_payloads;
    atomic_init(&shared.ready_threads, 0);
    atomic_init(&shared.producers_finished, false);
    atomic_init(&shared.failed, 0);
    atomic_init(&shared.start, false);

    if (CHANNEL_CREATE(&channel) != C_ChannelSuccess) {
        return run;
    }

    for (int index = 0; index < CHANNEL_CONSUMER_COUNT; ++index) {
        consumer_args[index].shared = &shared;
        consumer_args[index].core_index =
            (size_t)CHANNEL_PRODUCER_COUNT + (size_t)index;
        if (pthread_create(&consumers[index], 0, consumer_main, &consumer_args[index]) != 0) {
            atomic_store(&shared.failed, 1);
            break;
        }
        ++started_consumers;
    }

    const size_t messages_per_producer =
        (size_t)MESSAGES_PER_SAMPLE / (size_t)CHANNEL_PRODUCER_COUNT;
    for (int index = 0;
         index < CHANNEL_PRODUCER_COUNT && !atomic_load(&shared.failed);
         ++index) {
        producer_args[index].shared = &shared;
        producer_args[index].core_index = (size_t)index;
        producer_args[index].offset = (size_t)index * messages_per_producer;
        producer_args[index].count = messages_per_producer;
        if (pthread_create(&producers[index], 0, producer_main, &producer_args[index]) != 0) {
            atomic_store(&shared.failed, 1);
            break;
        }
        ++started_producers;
    }

    if (!atomic_load(&shared.failed) &&
        !wait_until_ready(&shared, CHANNEL_PRODUCER_COUNT + CHANNEL_CONSUMER_COUNT)) {
        atomic_store(&shared.failed, 1);
    }
    shared.start_us = now_us();
    if (shared.start_us == 0) {
        atomic_store(&shared.failed, 1);
    }
    atomic_store(&shared.start, true);

    for (int index = 0; index < started_producers; ++index) {
        if (pthread_join(producers[index], 0) != 0) {
            atomic_store(&shared.failed, 1);
        }
#if CHANNEL_USE_PRODUCER_TOKEN
        if (CHANNEL_PRODUCER_TOKEN_DESTROY(&producer_args[index].token) !=
            C_ChannelSuccess) {
            atomic_store(&shared.failed, 1);
        }
#endif
    }
    atomic_store(&shared.producers_finished, true);
#if CHANNEL_HAS_CLOSE
    if (CHANNEL_CLOSE(&channel) != C_ChannelSuccess) {
        atomic_store(&shared.failed, 1);
    }
#endif
    for (int index = 0; index < started_consumers; ++index) {
        if (pthread_join(consumers[index], 0) != 0) {
            atomic_store(&shared.failed, 1);
        }
#if CHANNEL_USE_CONSUMER_TOKEN
        if (CHANNEL_CONSUMER_TOKEN_DESTROY(&consumer_args[index].token) !=
            C_ChannelSuccess) {
            atomic_store(&shared.failed, 1);
        }
#endif
    }

    const int64_t finished_us = now_us();
    run.elapsed_us = finished_us > 0 ? finished_us - shared.start_us : 0;
    for (int index = 0; index < started_producers; ++index) {
        run.sent += producer_args[index].sent;
    }
    for (int index = 0; index < started_consumers; ++index) {
        run.received += consumer_args[index].received;
        run.sum += consumer_args[index].sum;
    }

    const uint64_t expected_sum =
        ((uint64_t)(MESSAGES_PER_SAMPLE - 1) * (uint64_t)MESSAGES_PER_SAMPLE) / 2;
    run.valid = started_producers == CHANNEL_PRODUCER_COUNT &&
        started_consumers == CHANNEL_CONSUMER_COUNT &&
        !atomic_load(&shared.failed) &&
        run.sent == MESSAGES_PER_SAMPLE &&
        run.received == MESSAGES_PER_SAMPLE &&
        run.sum == expected_sum &&
        run.elapsed_us > 0 &&
        CHANNEL_EMPTY(&channel);
    if (run.valid) {
        run.messages_per_second =
            (double)run.received * 1000000.0 / (double)run.elapsed_us;
    }

    if (CHANNEL_DESTROY(&channel) != C_ChannelSuccess) {
        run.valid = 0;
    }
    return run;
}

static int compare_double(const void* lhs, const void* rhs)
{
    const double left = *(const double*)lhs;
    const double right = *(const double*)rhs;
    return left < right ? -1 : left > right ? 1 : 0;
}

int main(void)
{
    for (size_t index = 0; index < MESSAGES_PER_SAMPLE; ++index) {
        g_payloads[index] = index;
    }

    for (int sample = 0; sample < WARMUP_SAMPLES; ++sample) {
        if (!run_sample().valid) {
            return 1;
        }
    }

    double samples[MEASURED_SAMPLES] = {0};
    for (int sample = 0; sample < MEASURED_SAMPLES; ++sample) {
        const RunResult run = run_sample();
        if (!run.valid) {
            return 2;
        }
        samples[sample] = run.messages_per_second;
    }
    qsort(samples, MEASURED_SAMPLES, sizeof(samples[0]), compare_double);

    if (printf("channel_throughput family=%s producers=%d consumers=%d capacity=%zu messages=%d median_msg_s=%.2f min_msg_s=%.2f max_msg_s=%.2f\n",
               CHANNEL_FAMILY,
               CHANNEL_PRODUCER_COUNT,
               CHANNEL_CONSUMER_COUNT,
               (size_t)CHANNEL_CAPACITY,
               MESSAGES_PER_SAMPLE,
               samples[MEASURED_SAMPLES / 2],
               samples[0],
               samples[MEASURED_SAMPLES - 1]) < 0) {
        return 3;
    }
    return 0;
}
