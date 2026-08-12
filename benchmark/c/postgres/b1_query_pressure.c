#define _POSIX_C_SOURCE 200809L

#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-postgres-c/postgres_c.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    kDefaultClients = 4,
    kDefaultQueries = 1000,
    kDefaultWarmup = 32,
    kDefaultTimeoutMs = -1,
    kMaxClients = 256,
    kMaxIoSchedulers = 256,
};

typedef struct PostgresBenchConfig {
    const char* host;
    const char* user;
    const char* password;
    const char* database;
    const char* sql;
    uint16_t port;
    size_t clients;
    size_t queries;
    size_t warmup;
    int64_t timeout_ms;
    size_t io_schedulers;
} PostgresBenchConfig;

typedef struct PostgresBenchState PostgresBenchState;

typedef struct PostgresBenchWorker {
    PostgresBenchState* state;
    uint64_t* samples_ns;
    size_t sample_count;
    C_IOResult first_error;
    const char* first_error_stage;
} PostgresBenchWorker;

struct PostgresBenchState {
    PostgresBenchConfig config;
    galay_postgres_config_t* postgres_config;
    size_t expected_workers;
    _Atomic size_t launch;
    _Atomic size_t ready;
    _Atomic size_t completed;
    _Atomic int started;
    _Atomic int fatal;
    _Atomic int64_t started_ns;
    _Atomic int64_t finished_ns;
    _Atomic uint64_t succeeded;
    _Atomic uint64_t failed;
    _Atomic uint64_t response_bytes;
    _Atomic uint64_t cleanup_errors;
};

static const char* environment_or(const char* name, const char* fallback)
{
    const char* value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static int parse_positive(const char* text, size_t maximum, size_t* value)
{
    char* end = NULL;
    unsigned long long parsed;
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return 0;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed == 0 ||
        parsed > (unsigned long long)maximum) {
        return 0;
    }
    *value = (size_t)parsed;
    return 1;
}

static int parse_timeout(const char* text, int64_t* value)
{
    char* end = NULL;
    long long parsed;
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return 0;
    }
    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed == 0 || parsed < -1) {
        return 0;
    }
    *value = (int64_t)parsed;
    return 1;
}

static int parse_port(const char* text, uint16_t* port)
{
    size_t parsed = 0;
    if (!parse_positive(text, UINT16_MAX, &parsed)) {
        return 0;
    }
    *port = (uint16_t)parsed;
    return 1;
}

static int64_t monotonic_ns(void)
{
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0 || timestamp.tv_sec < 0 ||
        (uint64_t)timestamp.tv_sec > (uint64_t)INT64_MAX / 1000000000ULL) {
        return -1;
    }
    return (int64_t)timestamp.tv_sec * 1000000000LL + (int64_t)timestamp.tv_nsec;
}

static void remember_error(PostgresBenchWorker* worker,
                           const char* stage,
                           C_IOResult result)
{
    if (worker->first_error_stage == NULL) {
        worker->first_error_stage = stage;
        worker->first_error = result;
    }
}

static C_IOResult benchmark_error(C_IOResultCode code, galay_status_t status)
{
    C_IOResult result = {code, 0, 0, (int64_t)status, NULL};
    return result;
}

static void add_counter(_Atomic uint64_t* counter, uint64_t value)
{
    /* Aggregate metrics do not need the counter's previous value. */
    (void)atomic_fetch_add_explicit(counter, value, memory_order_relaxed);
}

static int wait_for_launch(PostgresBenchWorker* worker)
{
    PostgresBenchState* state = worker->state;
    while (atomic_load_explicit(&state->launch, memory_order_acquire) == 0) {
        C_IOResult yielded = galay_coro_yield();
        if (yielded.code != C_IOResultOk) {
            remember_error(worker, "launch-yield", yielded);
            atomic_store_explicit(&state->fatal, 1, memory_order_release);
            return 0;
        }
    }
    return atomic_load_explicit(&state->fatal, memory_order_acquire) == 0;
}

static int wait_for_start(PostgresBenchWorker* worker)
{
    PostgresBenchState* state = worker->state;
    while (atomic_load_explicit(&state->started, memory_order_acquire) == 0) {
        C_IOResult yielded = galay_coro_yield();
        if (yielded.code != C_IOResultOk) {
            remember_error(worker, "start-yield", yielded);
            atomic_store_explicit(&state->fatal, 1, memory_order_release);
            return 0;
        }
    }
    return atomic_load_explicit(&state->fatal, memory_order_acquire) == 0;
}

static int collect_result_bytes(const galay_postgres_result_set_t* result,
                                uint64_t* response_bytes)
{
    size_t field_count = 0;
    size_t row_count = 0;
    if (result == NULL || response_bytes == NULL ||
        galay_postgres_result_set_field_count(result, &field_count) != GALAY_OK ||
        galay_postgres_result_set_row_count(result, &row_count) != GALAY_OK) {
        return 0;
    }
    for (size_t row = 0; row < row_count; ++row) {
        for (size_t column = 0; column < field_count; ++column) {
            galay_postgres_value_view_t value = {0};
            if (galay_postgres_result_set_value(result, row, column, &value) != GALAY_OK ||
                (value.is_null == GALAY_FALSE && value.data == NULL)) {
                return 0;
            }
            if (value.is_null == GALAY_FALSE) {
                *response_bytes += value.data_len;
            }
        }
    }
    return 1;
}

static void complete_measurement(PostgresBenchWorker* worker)
{
    PostgresBenchState* state = worker->state;
    const size_t previous = atomic_fetch_add_explicit(&state->completed, 1, memory_order_acq_rel);
    if (previous + 1 == state->expected_workers) {
        const int64_t finished = monotonic_ns();
        atomic_store_explicit(&state->finished_ns, finished, memory_order_release);
        if (finished < 0) {
            atomic_store_explicit(&state->fatal, 1, memory_order_release);
        }
    }
}

static void postgres_worker_entry(void* argument)
{
    PostgresBenchWorker* worker = (PostgresBenchWorker*)argument;
    PostgresBenchState* state = worker->state;
    galay_postgres_client_t* client = NULL;
    galay_postgres_result_set_t* reusable_result = NULL;
    int connected = 0;

    if (!wait_for_launch(worker)) {
        complete_measurement(worker);
        return;
    }
    const galay_status_t client_status = galay_postgres_client_create(&client);
    const galay_status_t result_status = client_status == GALAY_OK
        ? galay_postgres_result_set_create(&reusable_result)
        : client_status;
    if (client_status != GALAY_OK || client == NULL ||
        result_status != GALAY_OK || reusable_result == NULL) {
        remember_error(worker, "client-create",
                       benchmark_error(C_IOResultError, GALAY_OUT_OF_MEMORY));
        galay_postgres_client_destroy(client);
        client = NULL;
        add_counter(&state->failed, (uint64_t)state->config.queries);
    } else {
        C_IOResult io = galay_postgres_client_connect_async(
            client, state->postgres_config, state->config.timeout_ms);
        if (io.code != C_IOResultOk) {
            remember_error(worker, "connect", io);
            add_counter(&state->failed, (uint64_t)state->config.queries);
        } else {
            connected = 1;
            for (size_t index = 0; index < state->config.warmup; ++index) {
                io = galay_postgres_client_query_into_async(client,
                                                            state->config.sql,
                                                            state->config.timeout_ms,
                                                            reusable_result);
                if (io.code != C_IOResultOk) {
                    remember_error(worker, "warmup-query", io);
                    break;
                }
            }
            if (worker->first_error_stage != NULL) {
                add_counter(&state->failed, (uint64_t)state->config.queries);
            }
        }
    }

    const size_t previous_ready = atomic_fetch_add_explicit(&state->ready, 1, memory_order_acq_rel);
    if (previous_ready + 1 == state->expected_workers) {
        const int64_t started = monotonic_ns();
        atomic_store_explicit(&state->started_ns, started, memory_order_release);
        if (started < 0) {
            atomic_store_explicit(&state->fatal, 1, memory_order_release);
        }
        atomic_store_explicit(&state->started, 1, memory_order_release);
    }
    const int can_measure = wait_for_start(worker);

    if (can_measure && worker->first_error_stage == NULL && client != NULL && connected) {
        uint64_t local_succeeded = 0;
        uint64_t local_failed = 0;
        uint64_t local_response_bytes = 0;
        for (size_t index = 0; index < state->config.queries; ++index) {
            const int64_t begin = monotonic_ns();
            C_IOResult io = galay_postgres_client_query_into_async(client,
                                                                   state->config.sql,
                                                                   state->config.timeout_ms,
                                                                   reusable_result);
            const int64_t end = monotonic_ns();
            if (begin < 0 || end < begin) {
                remember_error(worker, "query-clock",
                               benchmark_error(C_IOResultError, GALAY_INTERNAL_ERROR));
                ++local_failed;
                continue;
            }
            worker->samples_ns[worker->sample_count++] = (uint64_t)(end - begin);
            if (io.code != C_IOResultOk) {
                remember_error(worker, "query", io);
                ++local_failed;
                continue;
            }
            uint64_t response_bytes = 0;
            if (!collect_result_bytes(reusable_result, &response_bytes)) {
                remember_error(worker, "result-access",
                               benchmark_error(C_IOResultError, GALAY_PROTOCOL_ERROR));
                ++local_failed;
            } else {
                ++local_succeeded;
                local_response_bytes += response_bytes;
            }
        }
        add_counter(&state->succeeded, local_succeeded);
        add_counter(&state->failed, local_failed);
        add_counter(&state->response_bytes, local_response_bytes);
    }

    complete_measurement(worker);
    if (client != NULL) {
        if (connected) {
            C_IOResult closed = galay_postgres_client_close_async(client, state->config.timeout_ms);
            if (closed.code != C_IOResultOk) {
                add_counter(&state->cleanup_errors, 1);
                remember_error(worker, "close", closed);
            }
        }
        galay_postgres_client_destroy(client);
    }
    galay_postgres_result_set_destroy(reusable_result);
}

static int compare_u64(const void* left, const void* right)
{
    const uint64_t a = *(const uint64_t*)left;
    const uint64_t b = *(const uint64_t*)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static double percentile_ms(const uint64_t* samples, size_t count, double fraction)
{
    if (count == 0) {
        return 0.0;
    }
    const size_t index = (size_t)(fraction * (double)(count - 1));
    return (double)samples[index] / 1000000.0;
}

static int parse_args(PostgresBenchConfig* config, int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (index + 1 >= argc) {
            return 0;
        }
        const char* value = argv[++index];
        if (strcmp(argument, "--clients") == 0) {
            if (!parse_positive(value, kMaxClients, &config->clients)) return 0;
        } else if (strcmp(argument, "--queries") == 0) {
            if (!parse_positive(value, SIZE_MAX / sizeof(uint64_t), &config->queries)) return 0;
        } else if (strcmp(argument, "--warmup") == 0) {
            if (!parse_positive(value, SIZE_MAX / sizeof(uint64_t), &config->warmup)) return 0;
        } else if (strcmp(argument, "--io-schedulers") == 0) {
            if (!parse_positive(value, kMaxIoSchedulers, &config->io_schedulers)) return 0;
        } else if (strcmp(argument, "--timeout-ms") == 0) {
            if (!parse_timeout(value, &config->timeout_ms)) return 0;
        } else if (strcmp(argument, "--sql") == 0) {
            if (value[0] == '\0') return 0;
            config->sql = value;
        } else {
            return 0;
        }
    }
    return 1;
}

static int load_config(PostgresBenchConfig* config)
{
    size_t parsed = 0;
    config->host = environment_or("GALAY_POSTGRES_HOST", "127.0.0.1");
    config->user = environment_or("GALAY_POSTGRES_USER", "postgres");
    config->password = environment_or("GALAY_POSTGRES_PASSWORD", "postgres");
    config->database = environment_or("GALAY_POSTGRES_DB", "postgres");
    config->sql = environment_or("GALAY_POSTGRES_BENCH_SQL", "SELECT 1");
    config->port = 5432;
    config->clients = kDefaultClients;
    config->queries = kDefaultQueries;
    config->warmup = kDefaultWarmup;
    config->timeout_ms = kDefaultTimeoutMs;
    config->io_schedulers = 0;
    if (parse_port(getenv("GALAY_POSTGRES_PORT"), &config->port) == 0 &&
        getenv("GALAY_POSTGRES_PORT") != NULL) {
        config->port = 5432;
    }
    if (parse_positive(getenv("GALAY_POSTGRES_BENCH_CLIENTS"), kMaxClients, &parsed)) {
        config->clients = parsed;
    }
    if (parse_positive(getenv("GALAY_POSTGRES_BENCH_QUERIES"),
                       SIZE_MAX / sizeof(uint64_t), &parsed)) {
        config->queries = parsed;
    }
    if (parse_positive(getenv("GALAY_POSTGRES_BENCH_WARMUP"),
                       SIZE_MAX / sizeof(uint64_t), &parsed)) {
        config->warmup = parsed;
    }
    if (parse_positive(getenv("GALAY_POSTGRES_BENCH_IO_SCHEDULERS"), kMaxIoSchedulers, &parsed)) {
        config->io_schedulers = parsed;
    }
    if (parse_timeout(getenv("GALAY_POSTGRES_BENCH_TIMEOUT_MS"), &config->timeout_ms) == 0 &&
        getenv("GALAY_POSTGRES_BENCH_TIMEOUT_MS") != NULL) {
        config->timeout_ms = kDefaultTimeoutMs;
    }
    return config->clients > 0 && config->queries > 0 && config->warmup > 0 &&
        config->sql[0] != '\0';
}

static int print_usage(const char* program)
{
    return fprintf(stderr,
                   "usage: %s [--clients N] [--queries N] [--warmup N] "
                   "[--io-schedulers N] [--timeout-ms N] [--sql SQL]\n",
                   program);
}

static int initialize_postgres_config(PostgresBenchState* state)
{
    galay_status_t status = galay_postgres_config_create(&state->postgres_config);
    if (status != GALAY_OK) return 0;
    if ((status = galay_postgres_config_set_host(
             state->postgres_config, state->config.host)) != GALAY_OK ||
        (status = galay_postgres_config_set_port(
             state->postgres_config, state->config.port)) != GALAY_OK ||
        (status = galay_postgres_config_set_username(
             state->postgres_config, state->config.user)) != GALAY_OK ||
        (status = galay_postgres_config_set_password(
             state->postgres_config, state->config.password)) != GALAY_OK ||
        (status = galay_postgres_config_set_database(
             state->postgres_config, state->config.database)) != GALAY_OK ||
        (status = galay_postgres_config_validate(state->postgres_config)) != GALAY_OK) {
        galay_postgres_config_destroy(state->postgres_config);
        state->postgres_config = NULL;
        return 0;
    }
    return 1;
}

int main(int argc, char** argv)
{
    PostgresBenchConfig config = {0};
    if (!load_config(&config) || !parse_args(&config, argc, argv)) {
        return print_usage(argv[0]) < 0 ? 12 : 2;
    }
    if (config.io_schedulers == 0) {
        config.io_schedulers = config.clients;
    }
    if (config.io_schedulers < config.clients) {
        return fputs("io-schedulers must be greater than or equal to clients\n", stderr) == EOF
            ? 12 : 2;
    }
    if (config.clients > SIZE_MAX / config.queries ||
        config.clients * config.queries > SIZE_MAX / sizeof(uint64_t)) {
        return fputs("clients * queries is too large\n", stderr) == EOF ? 12 : 2;
    }
    PostgresBenchState state = {0};
    state.config = config;
    state.expected_workers = config.clients;
    atomic_init(&state.launch, 0);
    atomic_init(&state.ready, 0);
    atomic_init(&state.completed, 0);
    atomic_init(&state.started, 0);
    atomic_init(&state.fatal, 0);
    atomic_init(&state.started_ns, -1);
    atomic_init(&state.finished_ns, -1);
    atomic_init(&state.succeeded, 0);
    atomic_init(&state.failed, 0);
    atomic_init(&state.response_bytes, 0);
    atomic_init(&state.cleanup_errors, 0);
    if (!initialize_postgres_config(&state)) {
        return fputs("invalid PostgreSQL configuration\n", stderr) == EOF ? 12 : 3;
    }

    PostgresBenchWorker* workers = calloc(config.clients, sizeof(*workers));
    galay_coro_task_t* tasks = calloc(config.clients, sizeof(*tasks));
    if (workers == NULL || tasks == NULL) {
        free(tasks);
        free(workers);
        galay_postgres_config_destroy(state.postgres_config);
        return 4;
    }
    for (size_t index = 0; index < config.clients; ++index) {
        workers[index].state = &state;
        workers[index].samples_ns = calloc(config.queries, sizeof(uint64_t));
        if (workers[index].samples_ns == NULL) {
            for (size_t cleanup = 0; cleanup < index; ++cleanup) free(workers[cleanup].samples_ns);
            free(tasks);
            free(workers);
            galay_postgres_config_destroy(state.postgres_config);
            return 4;
        }
    }

    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = config.io_schedulers;
    runtime_config.compute_scheduler_count = 0;
    galay_kernel_runtime_t runtime = {0};
    int exit_code = 0;
    size_t spawned = 0;
    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        exit_code = 5;
        goto cleanup;
    }
    for (; spawned < config.clients; ++spawned) {
        if (galay_coro_spawn(&runtime, postgres_worker_entry, &workers[spawned], NULL,
                             &tasks[spawned]).code != C_IOResultOk) {
            break;
        }
    }
    if (spawned != config.clients) {
        state.expected_workers = spawned;
        atomic_store_explicit(&state.fatal, 1, memory_order_release);
    }
    atomic_store_explicit(&state.launch, 1, memory_order_release);
    for (size_t index = 0; index < spawned; ++index) {
        C_IOResult joined = galay_coro_join(&tasks[index], -1);
        if (joined.code != C_IOResultOk && exit_code == 0) exit_code = 6;
    }
    if (spawned != config.clients ||
        atomic_load_explicit(&state.fatal, memory_order_acquire) != 0 ||
        atomic_load_explicit(&state.succeeded, memory_order_relaxed) == 0 ||
        atomic_load_explicit(&state.failed, memory_order_relaxed) != 0 ||
        atomic_load_explicit(&state.cleanup_errors, memory_order_relaxed) != 0) {
        exit_code = exit_code == 0 ? 7 : exit_code;
    }

    size_t total_samples = 0;
    for (size_t index = 0; index < spawned; ++index) {
        total_samples += workers[index].sample_count;
    }
    if (exit_code == 0 && total_samples != 0) {
        uint64_t* samples = malloc(total_samples * sizeof(uint64_t));
        if (samples == NULL) {
            exit_code = 8;
        } else {
            size_t copied = 0;
            for (size_t index = 0; index < spawned; ++index) {
                /* memcpy has no failure result; its contract returns the destination. */
                (void)memcpy(samples + copied, workers[index].samples_ns,
                             workers[index].sample_count * sizeof(uint64_t));
                copied += workers[index].sample_count;
            }
            qsort(samples, total_samples, sizeof(uint64_t), compare_u64);
            const int64_t started = atomic_load_explicit(&state.started_ns, memory_order_acquire);
            const int64_t finished = atomic_load_explicit(&state.finished_ns, memory_order_acquire);
            const double elapsed_sec = started >= 0 && finished >= started
                ? (double)(finished - started) / 1000000000.0
                : 0.0;
            const uint64_t succeeded = atomic_load_explicit(&state.succeeded, memory_order_relaxed);
            const uint64_t failed = atomic_load_explicit(&state.failed, memory_order_relaxed);
            if (printf("=== Galay C PostgreSQL Query Pressure ===\n"
                       "host=%s port=%u database=%s clients=%zu io_schedulers=%zu timeout_ms=%" PRId64 "\n"
                       "warmup_queries_per_client=%zu queries_per_client=%zu sql=%s\n"
                       "success=%" PRIu64 " failed=%" PRIu64 " response_bytes=%" PRIu64 "\n"
                       "elapsed_sec=%.6f qps=%.2f p50_latency_ms=%.3f p95_latency_ms=%.3f "
                       "p99_latency_ms=%.3f\n",
                       config.host, (unsigned)config.port, config.database, config.clients,
                       config.io_schedulers, config.timeout_ms,
                       config.warmup, config.queries, config.sql,
                       succeeded, failed,
                       atomic_load_explicit(&state.response_bytes, memory_order_relaxed),
                       elapsed_sec,
                       elapsed_sec > 0.0 ? (double)succeeded / elapsed_sec : 0.0,
                       percentile_ms(samples, total_samples, 0.50),
                       percentile_ms(samples, total_samples, 0.95),
                       percentile_ms(samples, total_samples, 0.99)) < 0) {
                exit_code = 12;
            }
            free(samples);
        }
    }
    for (size_t index = 0; index < spawned; ++index) {
        if (workers[index].first_error_stage != NULL) {
            if (fprintf(stderr, "worker[%zu] stage=%s code=%s value=%" PRId64 "\n",
                        index, workers[index].first_error_stage,
                        galay_coro_ioresult_string(workers[index].first_error.code),
                        workers[index].first_error.value) < 0 && exit_code == 0) {
                exit_code = 12;
            }
        }
    }

cleanup:
    atomic_store_explicit(&state.launch, 1, memory_order_release);
    for (size_t index = 0; index < spawned; ++index) {
        if (tasks[index].task != NULL && galay_coro_destroy(&tasks[index]).code != C_IOResultOk &&
            exit_code == 0) {
            exit_code = 9;
        }
    }
    if (runtime.runtime != NULL) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 10;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 11;
        }
    }
    for (size_t index = 0; index < config.clients; ++index) free(workers[index].samples_ns);
    free(tasks);
    free(workers);
    galay_postgres_config_destroy(state.postgres_config);
    return exit_code;
}
