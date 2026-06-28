#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <arpa/inet.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum {
    UDP_DEFAULT_PAYLOAD_BYTES = 256,
    UDP_DEFAULT_DURATION_SECONDS = 5,
    UDP_DEFAULT_IO_SCHEDULERS = 2
};

typedef struct BenchmarkConfig {
    size_t payload_bytes;
    int duration_seconds;
    size_t io_schedulers;
} BenchmarkConfig;

typedef struct BenchmarkState {
    galay_kernel_runtime_t runtime;
    galay_kernel_udp_socket_t server;
    galay_kernel_udp_socket_t client;
    galay_coro_task_t recv_task;
    galay_coro_task_t send_task;
    C_Host server_local;
    atomic_int stop;
    atomic_ullong recv_datagrams;
    atomic_ullong send_datagrams;
    atomic_ullong recv_bytes;
    atomic_ullong send_bytes;
    atomic_ullong errors;
    char* payload;
    char* recv_buffer;
    size_t payload_bytes;
} BenchmarkState;

static void add_counter(atomic_ullong* counter, unsigned long long value)
{
    unsigned long long previous = atomic_fetch_add(counter, value);
    if (value != 0 && previous > ULLONG_MAX - value) {
        atomic_store(counter, ULLONG_MAX);
    }
}

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void recv_entry(void* arg)
{
    BenchmarkState* state = (BenchmarkState*)arg;
    while (!atomic_load(&state->stop)) {
        C_Host from = {0};
        C_IOResult result = galay_kernel_udp_socket_recvfrom(
            &state->server,
            state->recv_buffer,
            state->payload_bytes,
            &from,
            -1);
        if (result.code != C_IOResultOk) {
            if (!atomic_load(&state->stop)) {
                add_counter(&state->errors, 1);
            }
            break;
        }
        if (result.bytes == 0) {
            continue;
        }
        add_counter(&state->recv_datagrams, 1);
        add_counter(&state->recv_bytes, (unsigned long long)result.bytes);
    }
}

static void send_entry(void* arg)
{
    BenchmarkState* state = (BenchmarkState*)arg;
    while (!atomic_load(&state->stop)) {
        C_IOResult result = galay_kernel_udp_socket_sendto(
            &state->client,
            state->payload,
            state->payload_bytes,
            &state->server_local,
            1000);
        if (result.code != C_IOResultOk || result.bytes != state->payload_bytes) {
            add_counter(&state->errors, 1);
            break;
        }
        add_counter(&state->send_datagrams, 1);
        add_counter(&state->send_bytes, (unsigned long long)result.bytes);
    }
}

static int wake_receiver(const C_Host* endpoint)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint->port);
    int status = 0;
    if (inet_pton(AF_INET, endpoint->address, &addr.sin_addr) != 1 ||
        sendto(fd, NULL, 0, 0, (const struct sockaddr*)&addr, sizeof(addr)) < 0) {
        status = 1;
    }
    if (close(fd) != 0) {
        status = 1;
    }
    return status;
}

static int print_usage(const char* program)
{
    return printf("Usage: %s [-s payload_bytes] [-d duration_seconds] [--io-schedulers count>=2]\n", program) < 0
        ? 1
        : 0;
}

static int parse_args(int argc, char** argv, BenchmarkConfig* config)
{
    config->payload_bytes = UDP_DEFAULT_PAYLOAD_BYTES;
    config->duration_seconds = UDP_DEFAULT_DURATION_SECONDS;
    config->io_schedulers = UDP_DEFAULT_IO_SCHEDULERS;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            config->payload_bytes = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            config->duration_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--io-schedulers") == 0 && i + 1 < argc) {
            config->io_schedulers = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            return print_usage(argv[0]) == 0 ? 1 : 2;
        } else {
            return print_usage(argv[0]) == 0 ? 1 : 2;
        }
    }

    return config->payload_bytes == 0 ||
        config->duration_seconds <= 0 ||
        config->io_schedulers < 2;
}

int main(int argc, char** argv)
{
    BenchmarkConfig bench_config;
    if (parse_args(argc, argv, &bench_config) != 0) {
        return 1;
    }

    BenchmarkState state = {0};
    state.payload_bytes = bench_config.payload_bytes;
    atomic_init(&state.stop, 0);
    atomic_init(&state.recv_datagrams, 0);
    atomic_init(&state.send_datagrams, 0);
    atomic_init(&state.recv_bytes, 0);
    atomic_init(&state.send_bytes, 0);
    atomic_init(&state.errors, 0);

    state.payload = (char*)malloc(state.payload_bytes);
    state.recv_buffer = (char*)malloc(state.payload_bytes);
    if (state.payload == NULL || state.recv_buffer == NULL) {
        free(state.payload);
        free(state.recv_buffer);
        return 2;
    }
    for (size_t i = 0; i < state.payload_bytes; ++i) {
        state.payload[i] = (char)('a' + (i % 26));
    }

    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = bench_config.io_schedulers;
    runtime_config.compute_scheduler_count = 0;

    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    int exit_code = 0;

    if (printf("C UDP benchmark starting payload=%zu duration=%d io_schedulers=%zu\n",
               bench_config.payload_bytes,
               bench_config.duration_seconds,
               bench_config.io_schedulers) < 0 ||
        printf("meta: role=loopback io_mode=plain scenario=udp-datagram-throughput mode=coro-direct\n") < 0 ||
        fflush(stdout) != 0) {
        exit_code = 3;
        goto cleanup;
    }

    if (galay_kernel_runtime_create(&runtime_config, &state.runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&state.runtime) != C_RuntimeSuccess ||
        galay_kernel_udp_socket_create(&state.server, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_create(&state.client, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&state.server, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&state.client, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&state.server, &state.server_local) != C_UdpSocketSuccess) {
        exit_code = 4;
        goto cleanup;
    }

    C_IOResult recv_spawn = galay_coro_spawn(&state.runtime, recv_entry, &state, NULL, &state.recv_task);
    C_IOResult send_spawn = galay_coro_spawn(&state.runtime, send_entry, &state, NULL, &state.send_task);
    if (recv_spawn.code != C_IOResultOk || send_spawn.code != C_IOResultOk) {
        exit_code = 5;
        goto cleanup;
    }

    const int64_t start_us = now_us();
    unsigned int remaining = sleep((unsigned int)bench_config.duration_seconds);
    if (remaining != 0) {
        exit_code = 6;
    }
    atomic_store(&state.stop, 1);
    if (wake_receiver(&state.server_local) != 0 && exit_code == 0) {
        exit_code = 7;
    }
    C_IOResult send_join = galay_coro_join(&state.send_task, 3000);
    C_IOResult send_destroy = galay_coro_destroy(&state.send_task);
    C_IOResult recv_join = galay_coro_join(&state.recv_task, 3000);
    C_IOResult recv_destroy = galay_coro_destroy(&state.recv_task);
    if ((send_join.code != C_IOResultOk ||
         send_destroy.code != C_IOResultOk ||
         recv_join.code != C_IOResultOk ||
         recv_destroy.code != C_IOResultOk) &&
        exit_code == 0) {
        exit_code = 8;
    }
    const int64_t elapsed_us = now_us() - start_us;

    const unsigned long long recv_datagrams = atomic_load(&state.recv_datagrams);
    const unsigned long long send_datagrams = atomic_load(&state.send_datagrams);
    const unsigned long long recv_bytes = atomic_load(&state.recv_bytes);
    const unsigned long long send_bytes = atomic_load(&state.send_bytes);
    const unsigned long long errors = atomic_load(&state.errors);
    const double elapsed_seconds = elapsed_us > 0 ? (double)elapsed_us / 1000000.0 : 0.0;
    const double throughput_mb = elapsed_seconds > 0.0
        ? (double)recv_bytes / 1024.0 / 1024.0 / elapsed_seconds
        : 0.0;

    if (printf("udp_socket_throughput sent_datagrams=%llu recv_datagrams=%llu sent_mb=%.3f recv_mb=%.3f throughput_mb_s=%.3f errors=%llu\n",
               send_datagrams,
               recv_datagrams,
               (double)send_bytes / 1024.0 / 1024.0,
               (double)recv_bytes / 1024.0 / 1024.0,
               throughput_mb,
               errors) < 0 &&
        exit_code == 0) {
        exit_code = 9;
    }
    if ((errors != 0 || send_datagrams == 0 || recv_datagrams == 0) && exit_code == 0) {
        exit_code = 10;
    }

cleanup:
    atomic_store(&state.stop, 1);
    if (state.send_task.task != NULL) {
        if (galay_coro_join(&state.send_task, 0).code == C_IOResultOk) {
            if (galay_coro_destroy(&state.send_task).code != C_IOResultOk && exit_code == 0) {
                exit_code = 11;
            }
        }
    }
    if (state.recv_task.task != NULL) {
        if (wake_receiver(&state.server_local) != 0 && exit_code == 0) {
            exit_code = 12;
        }
        if (galay_coro_join(&state.recv_task, 3000).code == C_IOResultOk) {
            if (galay_coro_destroy(&state.recv_task).code != C_IOResultOk && exit_code == 0) {
                exit_code = 13;
            }
        } else if (exit_code == 0) {
            exit_code = 14;
        }
    }
    if (state.client.socket != NULL &&
        galay_kernel_udp_socket_destroy(&state.client) != C_UdpSocketSuccess &&
        exit_code == 0) {
        exit_code = 15;
    }
    if (state.server.socket != NULL &&
        galay_kernel_udp_socket_destroy(&state.server) != C_UdpSocketSuccess &&
        exit_code == 0) {
        exit_code = 16;
    }
    if (state.runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&state.runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 17;
    }
    if (state.runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&state.runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 18;
    }
    free(state.payload);
    free(state.recv_buffer);
    return exit_code;
}
