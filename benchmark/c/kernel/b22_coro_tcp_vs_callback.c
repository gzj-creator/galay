#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_coro_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    kDefaultPayloadBytes = 1024,
    kDefaultDurationSeconds = 2,
    kDefaultIoSchedulers = 1,
    kMaxSamples = 200000,
};

typedef struct BenchConfig {
    size_t payload_bytes;
    int duration_seconds;
    size_t io_schedulers;
} BenchConfig;

typedef struct Metrics {
    uint64_t requests;
    uint64_t errors;
    int64_t elapsed_ns;
    int samples;
    int64_t* latencies;
} Metrics;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int compare_i64(const void* lhs, const void* rhs)
{
    int64_t a = *(const int64_t*)lhs;
    int64_t b = *(const int64_t*)rhs;
    return (a > b) - (a < b);
}

static double percentile_us(const int64_t* sorted, int count, double percentile)
{
    if (count <= 0) {
        return 0.0;
    }
    int index = (int)((double)(count - 1) * percentile);
    if (index < 0) {
        index = 0;
    }
    if (index >= count) {
        index = count - 1;
    }
    return (double)sorted[index] / 1000.0;
}

static void print_metrics(const char* mode, const BenchConfig* config, const Metrics* metrics)
{
    const double seconds = metrics->elapsed_ns > 0
        ? (double)metrics->elapsed_ns / 1000000000.0
        : 0.0;
    const double qps = seconds > 0.0 ? (double)metrics->requests / seconds : 0.0;
    const double throughput = seconds > 0.0
        ? (double)(metrics->requests * config->payload_bytes * 2u) / seconds / 1024.0 / 1024.0
        : 0.0;
    printf("coro_tcp_vs_callback mode=%s io_schedulers=%zu connections=1 duration_sec=%d payload_bytes=%zu elapsed_ms=%.3f requests=%llu qps=%.2f throughput_mb_per_sec=%.3f p50_us=%.2f p90_us=%.2f p99_us=%.2f errors=%llu\n",
           mode,
           config->io_schedulers,
           config->duration_seconds,
           config->payload_bytes,
           (double)metrics->elapsed_ns / 1000000.0,
           (unsigned long long)metrics->requests,
           qps,
           throughput,
           percentile_us(metrics->latencies, metrics->samples, 0.50),
           percentile_us(metrics->latencies, metrics->samples, 0.90),
           percentile_us(metrics->latencies, metrics->samples, 0.99),
           (unsigned long long)metrics->errors);
}

static int send_all_fd(int fd, const char* data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n <= 0) {
            return 1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all_fd(int fd, char* data, size_t length)
{
    size_t received = 0;
    while (received < length) {
        ssize_t n = recv(fd, data + received, length - received, 0);
        if (n <= 0) {
            return 1;
        }
        received += (size_t)n;
    }
    return 0;
}

static int create_listener(galay_kernel_tcp_socket_t* listener, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_kernel_tcp_socket_create(listener, C_IPTypeIPV4) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_bind(listener, &bind_host) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_listen(listener, 16) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_local_endpoint(listener, local) == C_TcpSocketSuccess &&
        local->port != 0
        ? 0
        : 1;
}

static int connect_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void fill_payload(char* payload, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        payload[i] = (char)('a' + (i % 26));
    }
}

static int run_client_loop(uint16_t port,
                           const BenchConfig* config,
                           char* payload,
                           char* response,
                           Metrics* metrics)
{
    int fd = connect_client(port);
    if (fd < 0) {
        ++metrics->errors;
        return 1;
    }

    const int64_t deadline = now_ns() + (int64_t)config->duration_seconds * 1000000000LL;
    const int64_t start_ns = now_ns();
    while (now_ns() < deadline) {
        const int64_t begin = now_ns();
        if (send_all_fd(fd, payload, config->payload_bytes) != 0 ||
            recv_all_fd(fd, response, config->payload_bytes) != 0 ||
            memcmp(payload, response, config->payload_bytes) != 0) {
            ++metrics->errors;
            break;
        }
        const int64_t end = now_ns();
        if (metrics->samples < kMaxSamples) {
            metrics->latencies[metrics->samples++] = end - begin;
        }
        ++metrics->requests;
    }
    metrics->elapsed_ns = now_ns() - start_ns;
    close(fd);
    return 0;
}

typedef struct DirectServer {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    char* buffer;
    size_t payload_bytes;
    atomic_int* stop;
    uint64_t errors;
} DirectServer;

static int send_all_coro(galay_kernel_tcp_socket_t* socket, const char* data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        C_IOResult result = galay_coro_tcp_send(socket, data + sent, length - sent, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        sent += result.bytes;
    }
    return 0;
}

static int recv_all_coro(galay_kernel_tcp_socket_t* socket, char* data, size_t length)
{
    size_t received = 0;
    while (received < length) {
        C_IOResult result = galay_coro_tcp_recv(socket, data + received, length - received, 1000);
        if (result.code == C_IOResultEof && received == 0) {
            return 2;
        }
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        received += result.bytes;
    }
    return 0;
}

static void direct_server_entry(void* arg)
{
    DirectServer* server = (DirectServer*)arg;
    C_IOResult accepted = galay_coro_tcp_accept(server->listener, &server->accepted, 5000);
    if (accepted.code != C_IOResultOk) {
        ++server->errors;
        return;
    }

    for (;;) {
        int recv_status = recv_all_coro(&server->accepted, server->buffer, server->payload_bytes);
        if (recv_status == 2) {
            break;
        }
        if (recv_status != 0) {
            ++server->errors;
            break;
        }
        if (send_all_coro(&server->accepted, server->buffer, server->payload_bytes) != 0) {
            ++server->errors;
            break;
        }
    }
    (void)galay_coro_tcp_close(&server->accepted, 1000);
}

static int run_direct_mode(const BenchConfig* config,
                           char* payload,
                           char* response,
                           char* server_buffer,
                           Metrics* metrics)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = config->io_schedulers;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    galay_coro_task_t server_task = {0};
    int exit_code = 0;
    atomic_int stop;
    atomic_init(&stop, 0);
    DirectServer server = {
        .listener = &listener,
        .accepted = {0},
        .buffer = server_buffer,
        .payload_bytes = config->payload_bytes,
        .stop = &stop,
        .errors = 0,
    };
    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, &local) != 0) {
        ++metrics->errors;
        exit_code = 1;
        goto cleanup;
    }

    if (galay_coro_spawn(&runtime, direct_server_entry, &server, 0, &server_task).code != C_IOResultOk) {
        ++metrics->errors;
        exit_code = 2;
        goto cleanup;
    }

    (void)run_client_loop(local.port, config, payload, response, metrics);
    atomic_store(&stop, 1);
    if (galay_coro_join(&server_task, 3000).code != C_IOResultOk) {
        ++metrics->errors;
    }
    if (galay_coro_destroy(&server_task).code != C_IOResultOk) {
        ++metrics->errors;
    }
    metrics->errors += server.errors;
    exit_code = 0;
    
cleanup:
    atomic_store(&stop, 1);
    if (server_task.task != 0) {
        if (galay_coro_join(&server_task, 3000).code == C_IOResultOk) {
            (void)galay_coro_destroy(&server_task);
        }
    }
    if (server.accepted.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&server.accepted);
    }
    (void)galay_kernel_tcp_socket_destroy(&listener);
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    qsort(metrics->latencies, (size_t)metrics->samples, sizeof(int64_t), compare_i64);
    return exit_code;
}

typedef struct CallbackServer {
    galay_kernel_runtime_t* runtime;
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    char* buffer;
    size_t payload_bytes;
    atomic_int done;
    uint64_t errors;
} CallbackServer;

static void callback_post_recv(CallbackServer* server);

static void callback_on_send(galay_kernel_tcp_send_result_t* result, void* ctx)
{
    CallbackServer* server = (CallbackServer*)ctx;
    if (result == 0 || result->code != C_TcpSocketSuccess || result->bytes == 0) {
        ++server->errors;
        atomic_store(&server->done, 1);
        return;
    }
    callback_post_recv(server);
}

static int callback_on_recv(galay_kernel_tcp_recv_result_t* result, void* ctx)
{
    CallbackServer* server = (CallbackServer*)ctx;
    if (result == 0 || result->code != C_TcpSocketSuccess || result->bytes == 0) {
        atomic_store(&server->done, 1);
        return 1;
    }
    if (galay_kernel_tcp_socket_send(server->runtime,
                                     &server->accepted,
                                     result->buffer,
                                     result->bytes,
                                     callback_on_send,
                                     server) != C_TcpSocketSuccess) {
        ++server->errors;
        atomic_store(&server->done, 1);
    }
    return 1;
}

static void callback_post_recv(CallbackServer* server)
{
    if (galay_kernel_tcp_socket_recv_loop(server->runtime,
                                          &server->accepted,
                                          server->buffer,
                                          server->payload_bytes,
                                          callback_on_recv,
                                          server) != C_TcpSocketSuccess) {
        ++server->errors;
        atomic_store(&server->done, 1);
    }
}

static void callback_on_accept(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    CallbackServer* server = (CallbackServer*)ctx;
    if (result == 0 || result->code != C_TcpSocketSuccess) {
        ++server->errors;
        atomic_store(&server->done, 1);
        return;
    }
    server->accepted = result->socket;
    callback_post_recv(server);
}

static int run_callback_mode(const BenchConfig* config,
                             char* payload,
                             char* response,
                             char* server_buffer,
                             Metrics* metrics)
{
    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = config->io_schedulers;
    runtime_config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    int exit_code = 0;
    CallbackServer server = {
        .runtime = &runtime,
        .listener = &listener,
        .accepted = {0},
        .buffer = server_buffer,
        .payload_bytes = config->payload_bytes,
        .done = ATOMIC_VAR_INIT(0),
        .errors = 0,
    };
    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, &local) != 0) {
        ++metrics->errors;
        exit_code = 1;
        goto cleanup;
    }

    if (galay_kernel_tcp_socket_accept(&runtime, &listener, callback_on_accept, &server) !=
        C_TcpSocketSuccess) {
        ++metrics->errors;
        exit_code = 2;
        goto cleanup;
    }

    (void)run_client_loop(local.port, config, payload, response, metrics);
    const int64_t wait_deadline = now_ns() + 3000000000LL;
    while (!atomic_load(&server.done) && now_ns() < wait_deadline) {
        usleep(1000);
    }
    if (!atomic_load(&server.done)) {
        ++metrics->errors;
    }
    metrics->errors += server.errors;
cleanup:
    if (server.accepted.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&server.accepted);
    }
    (void)galay_kernel_tcp_socket_destroy(&listener);
    if (runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&runtime);
        (void)galay_kernel_runtime_destroy(&runtime);
    }
    qsort(metrics->latencies, (size_t)metrics->samples, sizeof(int64_t), compare_i64);
    return exit_code;
}

static int parse_args(int argc, char** argv, BenchConfig* config)
{
    config->payload_bytes = kDefaultPayloadBytes;
    config->duration_seconds = kDefaultDurationSeconds;
    config->io_schedulers = kDefaultIoSchedulers;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            config->payload_bytes = (size_t)strtoull(argv[++i], 0, 10);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            config->duration_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--io-schedulers") == 0 && i + 1 < argc) {
            config->io_schedulers = (size_t)strtoull(argv[++i], 0, 10);
        } else {
            return 1;
        }
    }
    return config->payload_bytes == 0 || config->duration_seconds <= 0 ||
        config->io_schedulers == 0;
}

int main(int argc, char** argv)
{
    BenchConfig config;
    if (parse_args(argc, argv, &config) != 0) {
        return 1;
    }

    char* payload = (char*)malloc(config.payload_bytes);
    char* response = (char*)malloc(config.payload_bytes);
    char* server_buffer = (char*)malloc(config.payload_bytes);
    Metrics direct = {
        .requests = 0,
        .errors = 0,
        .elapsed_ns = 0,
        .samples = 0,
        .latencies = (int64_t*)calloc(kMaxSamples, sizeof(int64_t)),
    };
    Metrics callback = {
        .requests = 0,
        .errors = 0,
        .elapsed_ns = 0,
        .samples = 0,
        .latencies = (int64_t*)calloc(kMaxSamples, sizeof(int64_t)),
    };
    if (payload == 0 || response == 0 || server_buffer == 0 ||
        direct.latencies == 0 || callback.latencies == 0) {
        free(payload);
        free(response);
        free(server_buffer);
        free(direct.latencies);
        free(callback.latencies);
        return 2;
    }
    fill_payload(payload, config.payload_bytes);

    int failed = run_direct_mode(&config, payload, response, server_buffer, &direct);
    memset(response, 0, config.payload_bytes);
    memset(server_buffer, 0, config.payload_bytes);
    failed |= run_callback_mode(&config, payload, response, server_buffer, &callback);

    print_metrics("coro-direct", &config, &direct);
    print_metrics("callback-task-bridge", &config, &callback);
    if (callback.requests > 0) {
        const double delta = ((double)direct.requests - (double)callback.requests) *
            100.0 / (double)callback.requests;
        printf("coro_tcp_vs_callback summary direct_requests=%llu callback_requests=%llu request_delta_percent=%.2f\n",
               (unsigned long long)direct.requests,
               (unsigned long long)callback.requests,
               delta);
    }

    failed |= direct.errors != 0 || callback.errors != 0;
    free(payload);
    free(response);
    free(server_buffer);
    free(direct.latencies);
    free(callback.latencies);
    return failed ? 3 : 0;
}
