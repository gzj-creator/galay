#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>

enum {
    TCP_SERVER_DEFAULT_PORT = 19080,
    TCP_SERVER_DEFAULT_IO_SCHEDULERS = 2,
    TCP_SERVER_BUFFER_BYTES = 4096
};

typedef struct ServerState ServerState;
typedef struct ServerSession ServerSession;

struct ServerState {
    galay_kernel_runtime_t runtime;
    galay_kernel_tcp_socket_t listener;
    atomic_int running;
    atomic_ullong total_connections;
    atomic_ullong total_requests;
    atomic_ullong total_bytes;
    atomic_ullong total_errors;
    atomic_ullong active_sessions;
    uint16_t port;
};

struct ServerSession {
    ServerState* server;
    galay_kernel_tcp_socket_t socket;
    char buffer[TCP_SERVER_BUFFER_BYTES];
    size_t send_offset;
    size_t send_length;
    int closing;
};

static ServerState* g_server = 0;

static void post_recv(ServerSession* session);
static void post_send(ServerSession* session);
static void close_session(ServerSession* session);

static void signal_handler(int signum)
{
    (void)signum;
    if (g_server != 0) {
        atomic_store(&g_server->running, 0);
    }
}

static void on_close(C_TcpSocketResultCode code, void* ctx)
{
    ServerSession* session = (ServerSession*)ctx;
    if (code != C_TcpSocketSuccess) {
        atomic_fetch_add(&session->server->total_errors, 1);
    }
    (void)galay_kernel_tcp_socket_destroy(&session->socket);
    atomic_fetch_sub(&session->server->active_sessions, 1);
    free(session);
}

static void close_session(ServerSession* session)
{
    if (session == 0 || session->closing) {
        return;
    }
    session->closing = 1;
    if (session->socket.socket != 0 &&
        galay_kernel_tcp_socket_close(&session->server->runtime, &session->socket, on_close, session) == C_TcpSocketSuccess) {
        return;
    }
    if (session->socket.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&session->socket);
    }
    atomic_fetch_sub(&session->server->active_sessions, 1);
    free(session);
}

static void on_send(galay_kernel_tcp_send_result_t* result, void* ctx)
{
    ServerSession* session = (ServerSession*)ctx;
    if (result == 0 || result->code != C_TcpSocketSuccess || result->bytes == 0) {
        atomic_fetch_add(&session->server->total_errors, 1);
        close_session(session);
        return;
    }

    session->send_offset += result->bytes;
    atomic_fetch_add(&session->server->total_bytes, (unsigned long long)result->bytes);
    if (session->send_offset < session->send_length) {
        post_send(session);
        return;
    }

    post_recv(session);
}

static void post_send(ServerSession* session)
{
    if (!atomic_load(&session->server->running)) {
        close_session(session);
        return;
    }

    const size_t remaining = session->send_length - session->send_offset;
    if (galay_kernel_tcp_socket_send(
            &session->server->runtime,
            &session->socket,
            session->buffer + session->send_offset,
            remaining,
            on_send,
            session) != C_TcpSocketSuccess) {
        atomic_fetch_add(&session->server->total_errors, 1);
        close_session(session);
    }
}

static void on_recv(galay_kernel_tcp_recv_result_t* result, void* ctx)
{
    ServerSession* session = (ServerSession*)ctx;
    if (session->closing || !atomic_load(&session->server->running)) {
        close_session(session);
        return;
    }
    if (result == 0 || result->code != C_TcpSocketSuccess || result->bytes == 0) {
        close_session(session);
        return;
    }

    atomic_fetch_add(&session->server->total_requests, 1);
    atomic_fetch_add(&session->server->total_bytes, (unsigned long long)result->bytes);
    session->send_offset = 0;
    session->send_length = result->bytes;
    post_send(session);
}

static void post_recv(ServerSession* session)
{
    if (!atomic_load(&session->server->running)) {
        close_session(session);
        return;
    }

    if (galay_kernel_tcp_socket_recv(
            &session->server->runtime,
            &session->socket,
            session->buffer,
            sizeof(session->buffer),
            on_recv,
            session) != C_TcpSocketSuccess) {
        atomic_fetch_add(&session->server->total_errors, 1);
        close_session(session);
    }
}

static int on_accept(galay_kernel_tcp_accept_result_t* result, void* ctx)
{
    ServerState* server = (ServerState*)ctx;
    if (!atomic_load(&server->running)) {
        if (result != 0 && result->code == C_TcpSocketSuccess) {
            (void)galay_kernel_tcp_socket_destroy(&result->socket);
        }
        return 1;
    }

    if (result == 0 || result->code != C_TcpSocketSuccess || result->socket.socket == 0) {
        atomic_fetch_add(&server->total_errors, 1);
        return 1;
    }

    ServerSession* session = (ServerSession*)calloc(1, sizeof(ServerSession));
    if (session == 0) {
        atomic_fetch_add(&server->total_errors, 1);
        (void)galay_kernel_tcp_socket_destroy(&result->socket);
        return 0;
    }

    session->server = server;
    session->socket = result->socket;
    atomic_fetch_add(&server->total_connections, 1);
    atomic_fetch_add(&server->active_sessions, 1);
    post_recv(session);
    return 0;
}

static void* stats_thread_main(void* arg)
{
    ServerState* server = (ServerState*)arg;
    unsigned long long last_requests = 0;
    unsigned long long last_bytes = 0;

    while (atomic_load(&server->running)) {
        sleep(1);
        unsigned long long requests = atomic_load(&server->total_requests);
        unsigned long long bytes = atomic_load(&server->total_bytes);
        unsigned long long connections = atomic_load(&server->total_connections);
        unsigned long long errors = atomic_load(&server->total_errors);
        unsigned long long delta_requests = requests - last_requests;
        unsigned long long delta_bytes = bytes - last_bytes;

        printf("[Stats] Connections: %llu | Requests/s: %llu | Throughput: %.3f MB/s | Total Requests: %llu | Errors: %llu\n",
               connections,
               delta_requests,
               (double)delta_bytes / 1024.0 / 1024.0,
               requests,
               errors);
        fflush(stdout);

        last_requests = requests;
        last_bytes = bytes;
    }
    return 0;
}

static void wake_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1) {
        (void)connect(fd, (const struct sockaddr*)&addr, sizeof(addr));
    }
    close(fd);
}

int main(int argc, char** argv)
{
    ServerState server;
    memset(&server, 0, sizeof(server));
    server.port = TCP_SERVER_DEFAULT_PORT;
    size_t io_schedulers = TCP_SERVER_DEFAULT_IO_SCHEDULERS;
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
    atomic_init(&server.total_connections, 0);
    atomic_init(&server.total_requests, 0);
    atomic_init(&server.total_bytes, 0);
    atomic_init(&server.total_errors, 0);
    atomic_init(&server.active_sessions, 0);
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

    printf("C TCP benchmark server starting on port %u\n", server.port);
    printf("meta: role=server io_mode=plain scenario=tcp-echo io_schedulers=%zu compute_schedulers=0 mode=callback-loop-accept\n", io_schedulers);
    fflush(stdout);

    if (galay_kernel_runtime_create(&config, &server.runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&server.runtime) != C_RuntimeSuccess ||
        galay_kernel_tcp_socket_create(&server.listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(&server.listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&server.listener, 1024) != C_TcpSocketSuccess) {
        return 1;
    }

    if (pthread_create(&stats_thread, 0, stats_thread_main, &server) == 0) {
        stats_started = 1;
    }

    if (galay_kernel_tcp_socket_accept_loop(&server.runtime, &server.listener, on_accept, &server) != C_TcpSocketSuccess) {
        atomic_fetch_add(&server.total_errors, 1);
        exit_code = 2;
        atomic_store(&server.running, 0);
    }
    while (atomic_load(&server.running)) {
        sleep(1);
    }

    wake_listener(server.port);
    for (int i = 0; i < 3000 && atomic_load(&server.active_sessions) != 0; ++i) {
        usleep(1000);
    }

    if (stats_started) {
        (void)pthread_join(stats_thread, 0);
    }

    if (server.listener.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&server.listener);
    }
    if (server.runtime.runtime != 0) {
        (void)galay_kernel_runtime_stop(&server.runtime);
        (void)galay_kernel_runtime_destroy(&server.runtime);
    }

    printf("tcp_socket_server_throughput connections=%llu requests=%llu throughput_mb=%.3f errors=%llu\n",
           atomic_load(&server.total_connections),
           atomic_load(&server.total_requests),
           (double)atomic_load(&server.total_bytes) / 1024.0 / 1024.0,
           atomic_load(&server.total_errors));
    return exit_code;
}
