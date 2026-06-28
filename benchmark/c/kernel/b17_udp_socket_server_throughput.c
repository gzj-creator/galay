#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    UDP_SERVER_DEFAULT_PORT = 9090,
    UDP_SERVER_DEFAULT_IO_SCHEDULERS = 2,
    UDP_SERVER_BUFFER_BYTES = 65536
};

typedef struct ServerState {
    galay_kernel_runtime_t runtime;
    galay_kernel_udp_socket_t socket;
    galay_coro_task_t task;
    atomic_int running;
    atomic_ullong total_received;
    atomic_ullong total_sent;
    atomic_ullong total_recv_bytes;
    atomic_ullong total_send_bytes;
    atomic_ullong total_errors;
    uint16_t port;
    char buffer[UDP_SERVER_BUFFER_BYTES];
} ServerState;

static ServerState* g_server = NULL;

static void add_counter(atomic_ullong* counter, unsigned long long value)
{
    unsigned long long previous = atomic_fetch_add(counter, value);
    if (value != 0 && previous > ULLONG_MAX - value) {
        atomic_store(counter, ULLONG_MAX);
    }
}

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        if (g_server != NULL) {
            atomic_store(&g_server->running, 0);
        }
    }
}

static void server_entry(void* arg)
{
    ServerState* server = (ServerState*)arg;
    while (atomic_load(&server->running)) {
        C_Host peer = {0};
        C_IOResult received = galay_kernel_udp_socket_recvfrom(
            &server->socket,
            server->buffer,
            sizeof(server->buffer),
            &peer,
            -1);
        if (received.code != C_IOResultOk) {
            if (atomic_load(&server->running)) {
                add_counter(&server->total_errors, 1);
            }
            break;
        }
        if (received.bytes == 0) {
            continue;
        }
        add_counter(&server->total_received, 1);
        add_counter(&server->total_recv_bytes, (unsigned long long)received.bytes);

        C_IOResult sent = galay_kernel_udp_socket_sendto(
            &server->socket,
            server->buffer,
            received.bytes,
            &peer,
            1000);
        if (sent.code != C_IOResultOk || sent.bytes != received.bytes) {
            add_counter(&server->total_errors, 1);
            break;
        }
        add_counter(&server->total_sent, 1);
        add_counter(&server->total_send_bytes, (unsigned long long)sent.bytes);
    }

    C_IOResult closed = galay_kernel_udp_socket_close(&server->socket, 1000);
    if (closed.code != C_IOResultOk) {
        add_counter(&server->total_errors, 1);
    }
}

static void* stats_thread_main(void* arg)
{
    ServerState* server = (ServerState*)arg;
    unsigned long long last_received = 0;
    unsigned long long last_sent = 0;
    unsigned long long last_recv_bytes = 0;
    unsigned long long last_send_bytes = 0;

    while (atomic_load(&server->running)) {
        unsigned int remaining = sleep(1);
        if (remaining != 0) {
            continue;
        }
        const unsigned long long received = atomic_load(&server->total_received);
        const unsigned long long sent = atomic_load(&server->total_sent);
        const unsigned long long recv_bytes = atomic_load(&server->total_recv_bytes);
        const unsigned long long send_bytes = atomic_load(&server->total_send_bytes);
        const unsigned long long errors = atomic_load(&server->total_errors);

        if (printf("[Stats] Recv/s: %llu (%.3f MB/s) | Send/s: %llu (%.3f MB/s) | Total recv=%llu sent=%llu | Errors=%llu\n",
                   received - last_received,
                   (double)(recv_bytes - last_recv_bytes) / 1024.0 / 1024.0,
                   sent - last_sent,
                   (double)(send_bytes - last_send_bytes) / 1024.0 / 1024.0,
                   received,
                   sent,
                   errors) < 0) {
            add_counter(&server->total_errors, 1);
            atomic_store(&server->running, 0);
            break;
        }
        if (fflush(stdout) != 0) {
            add_counter(&server->total_errors, 1);
            atomic_store(&server->running, 0);
            break;
        }

        last_received = received;
        last_sent = sent;
        last_recv_bytes = recv_bytes;
        last_send_bytes = send_bytes;
    }
    return NULL;
}

static int wake_receiver(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    int status = 0;
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        sendto(fd, NULL, 0, 0, (const struct sockaddr*)&addr, sizeof(addr)) < 0) {
        status = 1;
    }
    if (close(fd) != 0) {
        status = 1;
    }
    return status;
}

int main(int argc, char** argv)
{
    ServerState server = {0};
    server.port = UDP_SERVER_DEFAULT_PORT;
    size_t io_schedulers = UDP_SERVER_DEFAULT_IO_SCHEDULERS;
    if (argc > 1) {
        server.port = (uint16_t)atoi(argv[1]);
    }
    if (argc > 2) {
        int parsed = atoi(argv[2]);
        if (parsed > 0) {
            io_schedulers = (size_t)parsed;
        }
    }

    atomic_init(&server.running, 1);
    atomic_init(&server.total_received, 0);
    atomic_init(&server.total_sent, 0);
    atomic_init(&server.total_recv_bytes, 0);
    atomic_init(&server.total_send_bytes, 0);
    atomic_init(&server.total_errors, 0);
    g_server = &server;

    if (signal(SIGINT, signal_handler) == SIG_ERR ||
        signal(SIGTERM, signal_handler) == SIG_ERR) {
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = io_schedulers;
    config.compute_scheduler_count = 0;

    C_Host bind_host = {C_IPTypeIPV4, "0.0.0.0", server.port};
    pthread_t stats_thread;
    int stats_started = 0;
    int exit_code = 0;

    if (printf("C UDP benchmark server starting on port %u\n", server.port) < 0 ||
        printf("meta: role=server io_mode=plain scenario=udp-echo io_schedulers=%zu compute_schedulers=0 mode=coro-direct\n",
               io_schedulers) < 0 ||
        fflush(stdout) != 0) {
        return 1;
    }

    if (galay_kernel_runtime_create(&config, &server.runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&server.runtime) != C_RuntimeSuccess ||
        galay_kernel_udp_socket_create(&server.socket, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&server.socket, &bind_host) != C_UdpSocketSuccess) {
        exit_code = 1;
        goto cleanup;
    }

    C_IOResult spawn_result = galay_coro_spawn(&server.runtime, server_entry, &server, NULL, &server.task);
    if (spawn_result.code != C_IOResultOk) {
        exit_code = 2;
        goto cleanup;
    }

    if (pthread_create(&stats_thread, NULL, stats_thread_main, &server) == 0) {
        stats_started = 1;
    }

    while (atomic_load(&server.running)) {
        unsigned int remaining = sleep(1);
        if (remaining != 0) {
            continue;
        }
    }

    if (wake_receiver(server.port) != 0 && exit_code == 0) {
        exit_code = 3;
    }
    C_IOResult join_result = galay_coro_join(&server.task, 3000);
    C_IOResult destroy_result = galay_coro_destroy(&server.task);
    if ((join_result.code != C_IOResultOk || destroy_result.code != C_IOResultOk) && exit_code == 0) {
        exit_code = 4;
    }

    if (stats_started) {
        if (pthread_join(stats_thread, NULL) != 0 && exit_code == 0) {
            exit_code = 5;
        }
        stats_started = 0;
    }

cleanup:
    atomic_store(&server.running, 0);
    if (stats_started && pthread_join(stats_thread, NULL) != 0 && exit_code == 0) {
        exit_code = 6;
    }
    if (server.task.task != NULL) {
        if (wake_receiver(server.port) != 0 && exit_code == 0) {
            exit_code = 7;
        }
        if (galay_coro_join(&server.task, 3000).code == C_IOResultOk) {
            if (galay_coro_destroy(&server.task).code != C_IOResultOk && exit_code == 0) {
                exit_code = 8;
            }
        } else if (exit_code == 0) {
            exit_code = 9;
        }
    }
    if (server.socket.socket != NULL &&
        galay_kernel_udp_socket_destroy(&server.socket) != C_UdpSocketSuccess &&
        exit_code == 0) {
        exit_code = 10;
    }
    if (server.runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&server.runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 11;
    }
    if (server.runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&server.runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 12;
    }

    if (printf("udp_socket_server_throughput received=%llu sent=%llu recv_mb=%.3f send_mb=%.3f errors=%llu\n",
               atomic_load(&server.total_received),
               atomic_load(&server.total_sent),
               (double)atomic_load(&server.total_recv_bytes) / 1024.0 / 1024.0,
               (double)atomic_load(&server.total_send_bytes) / 1024.0 / 1024.0,
               atomic_load(&server.total_errors)) < 0 &&
        exit_code == 0) {
        exit_code = 13;
    }
    return exit_code;
}
