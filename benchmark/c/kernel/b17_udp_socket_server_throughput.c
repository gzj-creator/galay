#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>

#include <arpa/inet.h>
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
    atomic_int running;
    atomic_int closing;
    atomic_int closed;
    atomic_ullong total_received;
    atomic_ullong total_sent;
    atomic_ullong total_recv_bytes;
    atomic_ullong total_send_bytes;
    atomic_ullong total_errors;
    uint16_t port;
    char buffer[UDP_SERVER_BUFFER_BYTES];
    C_Host last_peer;
    size_t send_length;
} ServerState;

static ServerState* g_server = 0;

static void post_recv(ServerState* server);
static void post_send(ServerState* server);
static void close_server(ServerState* server);

static void signal_handler(int signum)
{
    (void)signum;
    if (g_server != 0) {
        atomic_store(&g_server->running, 0);
    }
}

static void on_close(C_UdpSocketResultCode code, void* ctx)
{
    ServerState* server = (ServerState*)ctx;
    if (code != C_UdpSocketSuccess) {
        atomic_fetch_add(&server->total_errors, 1);
    }
    atomic_store(&server->closed, 1);
}

static void close_server(ServerState* server)
{
    int expected = 0;
    if (server == 0 || !atomic_compare_exchange_strong(&server->closing, &expected, 1)) {
        return;
    }

    if (server->socket.socket != 0 &&
        galay_kernel_udp_socket_close(&server->runtime, &server->socket, on_close, server) == C_UdpSocketSuccess) {
        return;
    }
    atomic_fetch_add(&server->total_errors, 1);
    atomic_store(&server->closed, 1);
}

static void on_send(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    ServerState* server = (ServerState*)ctx;
    if (atomic_load(&server->closing)) {
        return;
    }

    if (result == 0 || result->code != C_UdpSocketSuccess || result->bytes != server->send_length) {
        atomic_fetch_add(&server->total_errors, 1);
        close_server(server);
        return;
    }

    atomic_fetch_add(&server->total_sent, 1);
    atomic_fetch_add(&server->total_send_bytes, (unsigned long long)result->bytes);
    if (!atomic_load(&server->running)) {
        close_server(server);
        return;
    }
    post_recv(server);
}

static void post_send(ServerState* server)
{
    if (!atomic_load(&server->running)) {
        close_server(server);
        return;
    }

    if (galay_kernel_udp_socket_sendto(
            &server->runtime,
            &server->socket,
            server->buffer,
            server->send_length,
            &server->last_peer,
            on_send,
            server) != C_UdpSocketSuccess) {
        atomic_fetch_add(&server->total_errors, 1);
        close_server(server);
    }
}

static void on_recv(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    ServerState* server = (ServerState*)ctx;
    if (atomic_load(&server->closing)) {
        return;
    }

    if (result == 0 || result->code != C_UdpSocketSuccess) {
        if (atomic_load(&server->running)) {
            atomic_fetch_add(&server->total_errors, 1);
        }
        close_server(server);
        return;
    }

    if (result->bytes == 0) {
        if (atomic_load(&server->running)) {
            post_recv(server);
        } else {
            close_server(server);
        }
        return;
    }

    server->last_peer = result->from;
    server->send_length = result->bytes;
    atomic_fetch_add(&server->total_received, 1);
    atomic_fetch_add(&server->total_recv_bytes, (unsigned long long)result->bytes);
    post_send(server);
}

static void post_recv(ServerState* server)
{
    if (!atomic_load(&server->running)) {
        close_server(server);
        return;
    }

    if (galay_kernel_udp_socket_recvfrom(
            &server->runtime,
            &server->socket,
            server->buffer,
            sizeof(server->buffer),
            on_recv,
            server) != C_UdpSocketSuccess) {
        atomic_fetch_add(&server->total_errors, 1);
        close_server(server);
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
        sleep(1);
        const unsigned long long received = atomic_load(&server->total_received);
        const unsigned long long sent = atomic_load(&server->total_sent);
        const unsigned long long recv_bytes = atomic_load(&server->total_recv_bytes);
        const unsigned long long send_bytes = atomic_load(&server->total_send_bytes);
        const unsigned long long errors = atomic_load(&server->total_errors);

        printf("[Stats] Recv/s: %llu (%.3f MB/s) | Send/s: %llu (%.3f MB/s) | Total recv=%llu sent=%llu | Errors=%llu\n",
               received - last_received,
               (double)(recv_bytes - last_recv_bytes) / 1024.0 / 1024.0,
               sent - last_sent,
               (double)(send_bytes - last_send_bytes) / 1024.0 / 1024.0,
               received,
               sent,
               errors);
        fflush(stdout);

        last_received = received;
        last_sent = sent;
        last_recv_bytes = recv_bytes;
        last_send_bytes = send_bytes;
    }
    return 0;
}

static void wake_receiver(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1) {
        (void)sendto(fd, 0, 0, 0, (const struct sockaddr*)&addr, sizeof(addr));
    }
    close(fd);
}

int main(int argc, char** argv)
{
    ServerState server;
    memset(&server, 0, sizeof(server));
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
    atomic_init(&server.closing, 0);
    atomic_init(&server.closed, 0);
    atomic_init(&server.total_received, 0);
    atomic_init(&server.total_sent, 0);
    atomic_init(&server.total_recv_bytes, 0);
    atomic_init(&server.total_send_bytes, 0);
    atomic_init(&server.total_errors, 0);
    g_server = &server;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = io_schedulers;
    config.compute_scheduler_count = 0;

    C_Host bind_host = {C_IPTypeIPV4, "0.0.0.0", server.port};
    pthread_t stats_thread;
    int stats_started = 0;
    int exit_code = 0;

    printf("C UDP benchmark server starting on port %u\n", server.port);
    printf("meta: role=server io_mode=plain scenario=udp-echo io_schedulers=%zu compute_schedulers=0 mode=callback-chain\n",
           io_schedulers);
    fflush(stdout);

    if (galay_kernel_runtime_create(&config, &server.runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&server.runtime) != C_RuntimeSuccess ||
        galay_kernel_udp_socket_create(&server.socket, C_IPTypeIPV4) != C_UdpSocketSuccess ||
        galay_kernel_udp_socket_bind(&server.socket, &bind_host) != C_UdpSocketSuccess) {
        return 1;
    }

    if (pthread_create(&stats_thread, 0, stats_thread_main, &server) == 0) {
        stats_started = 1;
    }

    post_recv(&server);
    while (atomic_load(&server.running)) {
        sleep(1);
    }

    wake_receiver(server.port);
    for (int i = 0; i < 3000 && !atomic_load(&server.closed); ++i) {
        usleep(1000);
    }
    if (!atomic_load(&server.closed)) {
        close_server(&server);
        for (int i = 0; i < 3000 && !atomic_load(&server.closed); ++i) {
            usleep(1000);
        }
    }

    if (stats_started) {
        (void)pthread_join(stats_thread, 0);
    }

    if (!atomic_load(&server.closed)) {
        exit_code = 2;
    }
    if (server.socket.socket != 0) {
        (void)galay_kernel_udp_socket_destroy(&server.socket);
    }
    if (server.runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&server.runtime);
        (void)galay_kernel_runtime_destroy(&server.runtime);
    }

    printf("udp_socket_server_throughput received=%llu sent=%llu recv_mb=%.3f send_mb=%.3f errors=%llu\n",
           atomic_load(&server.total_received),
           atomic_load(&server.total_sent),
           (double)atomic_load(&server.total_recv_bytes) / 1024.0 / 1024.0,
           (double)atomic_load(&server.total_send_bytes) / 1024.0 / 1024.0,
           atomic_load(&server.total_errors));
    return exit_code;
}
