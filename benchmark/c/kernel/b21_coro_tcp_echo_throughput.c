#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
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

typedef struct DirectServer {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    char* buffer;
    size_t payload_bytes;
    atomic_int* stop;
    atomic_int ready;
    uint64_t errors;
} DirectServer;

typedef struct BenchConfig {
    size_t payload_bytes;
    int duration_seconds;
    size_t io_schedulers;
} BenchConfig;

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

static int send_all_coro(galay_kernel_tcp_socket_t* socket, const char* data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        C_IOResult result = galay_kernel_tcp_socket_send(socket, data + sent, length - sent, 1000);
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
        C_IOResult result = galay_kernel_tcp_socket_recv(socket, data + received, length - received, 1000);
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
    C_IOResult accepted = galay_kernel_tcp_socket_accept(server->listener, &server->accepted, NULL, 5000);
    if (accepted.code != C_IOResultOk) {
        ++server->errors;
        atomic_store(&server->ready, 1);
        return;
    }
    atomic_store(&server->ready, 1);

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

    (void)galay_kernel_tcp_socket_close(&server->accepted, 1000);
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

    int64_t* latencies = (int64_t*)calloc(kMaxSamples, sizeof(int64_t));
    char* payload = (char*)malloc(config.payload_bytes);
    char* response = (char*)malloc(config.payload_bytes);
    char* server_buffer = (char*)malloc(config.payload_bytes);
    if (latencies == 0 || payload == 0 || response == 0 || server_buffer == 0) {
        free(latencies);
        free(payload);
        free(response);
        free(server_buffer);
        return 2;
    }
    fill_payload(payload, config.payload_bytes);

    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = config.io_schedulers;
    runtime_config.compute_scheduler_count = 0;
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    galay_coro_task_t server_task = {0};
    int client_fd = -1;
    int exit_code = 0;
    atomic_int stop;
    atomic_init(&stop, 0);
    DirectServer server = {
        .listener = &listener,
        .accepted = {0},
        .buffer = server_buffer,
        .payload_bytes = config.payload_bytes,
        .stop = &stop,
        .ready = ATOMIC_VAR_INIT(0),
        .errors = 0,
    };

    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, &local) != 0) {
        exit_code = 3;
        goto cleanup;
    }

    if (galay_coro_spawn(&runtime, direct_server_entry, &server, 0, &server_task).code != C_IOResultOk) {
        exit_code = 4;
        goto cleanup;
    }

    client_fd = connect_client(local.port);
    if (client_fd < 0) {
        exit_code = 5;
        goto cleanup;
    }

    const int64_t deadline = now_ns() + (int64_t)config.duration_seconds * 1000000000LL;
    uint64_t requests = 0;
    uint64_t errors = 0;
    int samples = 0;
    const int64_t start_ns = now_ns();
    while (now_ns() < deadline) {
        const int64_t begin = now_ns();
        if (send_all_fd(client_fd, payload, config.payload_bytes) != 0 ||
            recv_all_fd(client_fd, response, config.payload_bytes) != 0 ||
            memcmp(payload, response, config.payload_bytes) != 0) {
            ++errors;
            break;
        }
        const int64_t end = now_ns();
        if (samples < kMaxSamples) {
            latencies[samples++] = end - begin;
        }
        ++requests;
    }
    const int64_t elapsed_ns = now_ns() - start_ns;
    atomic_store(&stop, 1);
    close(client_fd);
    client_fd = -1;
    if (galay_coro_join(&server_task, 3000).code != C_IOResultOk) {
        ++errors;
    }
    if (galay_coro_destroy(&server_task).code != C_IOResultOk) {
        ++errors;
    }

    qsort(latencies, (size_t)samples, sizeof(int64_t), compare_i64);
    const double seconds = elapsed_ns > 0 ? (double)elapsed_ns / 1000000000.0 : 0.0;
    const double qps = seconds > 0.0 ? (double)requests / seconds : 0.0;
    const double throughput = seconds > 0.0
        ? (double)(requests * config.payload_bytes * 2u) / seconds / 1024.0 / 1024.0
        : 0.0;
    errors += server.errors;

    printf("coro_tcp_echo_throughput mode=coro-direct io_schedulers=%zu connections=1 duration_sec=%d payload_bytes=%zu elapsed_ms=%.3f requests=%llu qps=%.2f throughput_mb_per_sec=%.3f p50_us=%.2f p90_us=%.2f p99_us=%.2f errors=%llu\n",
           config.io_schedulers,
           config.duration_seconds,
           config.payload_bytes,
           (double)elapsed_ns / 1000000.0,
           (unsigned long long)requests,
           qps,
           throughput,
           percentile_us(latencies, samples, 0.50),
           percentile_us(latencies, samples, 0.90),
               percentile_us(latencies, samples, 0.99),
               (unsigned long long)errors);
    
    exit_code = errors == 0 ? 0 : 6;
    
cleanup:
    atomic_store(&stop, 1);
    if (client_fd >= 0) {
        close(client_fd);
    }
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
    free(latencies);
    free(payload);
    free(response);
    free(server_buffer);
    return exit_code;
}
