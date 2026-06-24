#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>

#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
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
    C_Host server_local;
    atomic_int stop;
    atomic_int recv_done;
    atomic_int send_done;
    atomic_ullong recv_datagrams;
    atomic_ullong send_datagrams;
    atomic_ullong recv_bytes;
    atomic_ullong send_bytes;
    atomic_ullong errors;
    char* payload;
    char* recv_buffer;
    size_t payload_bytes;
} BenchmarkState;

typedef struct CloseState {
    atomic_int done;
    atomic_int code;
} CloseState;

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static int wait_done(atomic_int* done)
{
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load(done)) {
            return 0;
        }
        nanosleep(&pause, 0);
    }
    return 1;
}

static void on_close(C_UdpSocketResultCode code, void* ctx)
{
    CloseState* state = (CloseState*)ctx;
    atomic_store(&state->code, (int)code);
    atomic_store(&state->done, 1);
}

static int close_socket(galay_kernel_runtime_t* runtime, galay_kernel_udp_socket_t* socket)
{
    CloseState state;
    atomic_init(&state.done, 0);
    atomic_init(&state.code, (int)C_UdpSocketIOFailed);
    if (socket->socket == 0) {
        return 0;
    }
    if (galay_kernel_udp_socket_close(runtime, socket, on_close, &state) != C_UdpSocketSuccess) {
        return 1;
    }
    return wait_done(&state.done) != 0 ||
        atomic_load(&state.code) != (int)C_UdpSocketSuccess;
}

static void post_send(BenchmarkState* state);

static int on_recv_loop(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    BenchmarkState* state = (BenchmarkState*)ctx;
    if (result == 0 || result->code != C_UdpSocketSuccess) {
        atomic_fetch_add(&state->errors, 1);
        atomic_store(&state->recv_done, 1);
        return 1;
    }

    atomic_fetch_add(&state->recv_datagrams, 1);
    atomic_fetch_add(&state->recv_bytes, (unsigned long long)result->bytes);
    if (atomic_load(&state->stop)) {
        atomic_store(&state->recv_done, 1);
        return 1;
    }
    return 0;
}

static void on_send(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    BenchmarkState* state = (BenchmarkState*)ctx;
    if (result == 0 || result->code != C_UdpSocketSuccess || result->bytes != state->payload_bytes) {
        atomic_fetch_add(&state->errors, 1);
        atomic_store(&state->send_done, 1);
        return;
    }

    atomic_fetch_add(&state->send_datagrams, 1);
    atomic_fetch_add(&state->send_bytes, (unsigned long long)result->bytes);
    if (atomic_load(&state->stop)) {
        atomic_store(&state->send_done, 1);
        return;
    }
    post_send(state);
}

static void post_send(BenchmarkState* state)
{
    if (atomic_load(&state->stop)) {
        atomic_store(&state->send_done, 1);
        return;
    }
    if (galay_kernel_udp_socket_sendto(
            &state->runtime,
            &state->client,
            state->payload,
            state->payload_bytes,
            &state->server_local,
            on_send,
            state) != C_UdpSocketSuccess) {
        atomic_fetch_add(&state->errors, 1);
        atomic_store(&state->send_done, 1);
    }
}

static void wake_receiver(const C_Host* endpoint)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint->port);
    if (inet_pton(AF_INET, endpoint->address, &addr.sin_addr) == 1) {
        (void)sendto(fd, 0, 0, 0, (const struct sockaddr*)&addr, sizeof(addr));
    }
    close(fd);
}

static void print_usage(const char* program)
{
    printf("Usage: %s [-s payload_bytes] [-d duration_seconds] [--io-schedulers count>=2]\n", program);
}

static int parse_args(int argc, char** argv, BenchmarkConfig* config)
{
    config->payload_bytes = UDP_DEFAULT_PAYLOAD_BYTES;
    config->duration_seconds = UDP_DEFAULT_DURATION_SECONDS;
    config->io_schedulers = UDP_DEFAULT_IO_SCHEDULERS;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            config->payload_bytes = (size_t)strtoull(argv[++i], 0, 10);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            config->duration_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--io-schedulers") == 0 && i + 1 < argc) {
            config->io_schedulers = (size_t)strtoull(argv[++i], 0, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        } else {
            print_usage(argv[0]);
            return 1;
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

    BenchmarkState state;
    memset(&state, 0, sizeof(state));
    state.payload_bytes = bench_config.payload_bytes;
    atomic_init(&state.stop, 0);
    atomic_init(&state.recv_done, 0);
    atomic_init(&state.send_done, 0);
    atomic_init(&state.recv_datagrams, 0);
    atomic_init(&state.send_datagrams, 0);
    atomic_init(&state.recv_bytes, 0);
    atomic_init(&state.send_bytes, 0);
    atomic_init(&state.errors, 0);

    state.payload = (char*)malloc(state.payload_bytes);
    state.recv_buffer = (char*)malloc(state.payload_bytes);
    if (state.payload == 0 || state.recv_buffer == 0) {
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

    printf("C UDP benchmark starting payload=%zu duration=%d io_schedulers=%zu\n",
           bench_config.payload_bytes,
           bench_config.duration_seconds,
           bench_config.io_schedulers);
    printf("meta: role=loopback io_mode=plain scenario=udp-datagram-throughput mode=callback-loop\n");
    fflush(stdout);

    if (galay_kernel_runtime_create(&runtime_config, &state.runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&state.runtime) != C_RuntimeSuccess ||
        galay_kernel_udp_socket_create(&state.server, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_create(&state.client, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&state.server, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&state.client, &bind_host) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_local_endpoint(&state.server, &state.server_local) != C_UdpSocketSuccess) {
        exit_code = 3;
        goto cleanup;
    }

    if (galay_kernel_udp_socket_recvfrom_loop(
            &state.runtime,
            &state.server,
            state.recv_buffer,
            state.payload_bytes,
            on_recv_loop,
            &state) != C_UdpSocketSuccess) {
        exit_code = 4;
        goto cleanup;
    }
    post_send(&state);

    const int64_t start_us = now_us();
    sleep((unsigned int)bench_config.duration_seconds);
    atomic_store(&state.stop, 1);
    wake_receiver(&state.server_local);
    (void)wait_done(&state.send_done);
    (void)wait_done(&state.recv_done);
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

    printf("udp_socket_throughput sent_datagrams=%llu recv_datagrams=%llu sent_mb=%.3f recv_mb=%.3f throughput_mb_s=%.3f errors=%llu\n",
           send_datagrams,
           recv_datagrams,
           (double)send_bytes / 1024.0 / 1024.0,
           (double)recv_bytes / 1024.0 / 1024.0,
           throughput_mb,
           errors);
    if (errors != 0 || send_datagrams == 0 || recv_datagrams == 0) {
        exit_code = 5;
    }

cleanup:
    if (state.runtime.runtime != 0) {
        if (state.client.socket != 0) {
            (void)close_socket(&state.runtime, &state.client);
        }
        if (state.server.socket != 0) {
            (void)close_socket(&state.runtime, &state.server);
        }
    }
    if (state.client.socket != 0) {
        (void)galay_kernel_udp_socket_destroy(&state.client);
    }
    if (state.server.socket != 0) {
        (void)galay_kernel_udp_socket_destroy(&state.server);
    }
    if (state.runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&state.runtime);
        (void)galay_kernel_runtime_destroy(&state.runtime);
    }
    free(state.payload);
    free(state.recv_buffer);
    return exit_code;
}
