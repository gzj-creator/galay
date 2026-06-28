#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
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
    TCP_SERVER_DEFAULT_PORT = 19080,
    TCP_SERVER_DEFAULT_IO_SCHEDULERS = 2,
    TCP_SERVER_BUFFER_BYTES = 4096,
    TCP_SERVER_MAX_SESSIONS = 4096
};

typedef struct ServerState ServerState;

typedef struct ServerSession {
    ServerState* server;
    galay_kernel_tcp_socket_t socket;
    galay_coro_task_t task;
    char buffer[TCP_SERVER_BUFFER_BYTES];
    atomic_int active;
} ServerSession;

struct ServerState {
    galay_kernel_runtime_t runtime;
    galay_kernel_tcp_socket_t listener;
    galay_coro_task_t accept_task;
    atomic_int running;
    atomic_ullong total_connections;
    atomic_ullong total_requests;
    atomic_ullong total_bytes;
    atomic_ullong total_errors;
    atomic_ullong active_sessions;
    uint16_t port;
    ServerSession sessions[TCP_SERVER_MAX_SESSIONS];
};

static ServerState* g_server = NULL;

static void add_counter(atomic_ullong* counter, unsigned long long value)
{
    unsigned long long previous = atomic_fetch_add(counter, value);
    if (value != 0 && previous > ULLONG_MAX - value) {
        atomic_store(counter, ULLONG_MAX);
    }
}

static void subtract_counter(atomic_ullong* counter, unsigned long long value)
{
    unsigned long long previous = atomic_fetch_sub(counter, value);
    if (previous < value) {
        atomic_store(counter, 0);
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

static int send_all(galay_kernel_tcp_socket_t* socket, const char* buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        C_IOResult result = galay_kernel_tcp_socket_send(socket, buffer + sent, length - sent, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        sent += result.bytes;
    }
    return 0;
}

static void session_entry(void* arg)
{
    ServerSession* session = (ServerSession*)arg;
    ServerState* server = session->server;
    while (atomic_load(&server->running)) {
        C_IOResult received = galay_kernel_tcp_socket_recv(
            &session->socket,
            session->buffer,
            sizeof(session->buffer),
            1000);
        if (received.code == C_IOResultTimeout) {
            continue;
        }
        if (received.code == C_IOResultEof) {
            break;
        }
        if (received.code != C_IOResultOk || received.bytes == 0) {
            add_counter(&server->total_errors, 1);
            break;
        }

        add_counter(&server->total_requests, 1);
        add_counter(&server->total_bytes, (unsigned long long)received.bytes);
        if (send_all(&session->socket, session->buffer, received.bytes) != 0) {
            add_counter(&server->total_errors, 1);
            break;
        }
        add_counter(&server->total_bytes, (unsigned long long)received.bytes);
    }

    C_IOResult closed = galay_kernel_tcp_socket_close(&session->socket, 1000);
    if (closed.code != C_IOResultOk && closed.code != C_IOResultEof) {
        add_counter(&server->total_errors, 1);
    }
    if (galay_kernel_tcp_socket_destroy(&session->socket) != C_TcpSocketSuccess) {
        add_counter(&server->total_errors, 1);
    }
    atomic_store(&session->active, 0);
    subtract_counter(&server->active_sessions, 1);
}

static ServerSession* acquire_session(ServerState* server)
{
    for (int i = 0; i < TCP_SERVER_MAX_SESSIONS; ++i) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&server->sessions[i].active, &expected, 1)) {
            return &server->sessions[i];
        }
    }
    return NULL;
}

static void accept_entry(void* arg)
{
    ServerState* server = (ServerState*)arg;
    while (atomic_load(&server->running)) {
        galay_kernel_tcp_socket_t accepted = {0};
        C_IOResult result = galay_kernel_tcp_socket_accept(&server->listener, &accepted, NULL, 1000);
        if (result.code == C_IOResultTimeout) {
            continue;
        }
        if (result.code != C_IOResultOk || accepted.socket == NULL) {
            if (atomic_load(&server->running)) {
                add_counter(&server->total_errors, 1);
            }
            continue;
        }

        ServerSession* session = acquire_session(server);
        if (session == NULL) {
            add_counter(&server->total_errors, 1);
            if (galay_kernel_tcp_socket_destroy(&accepted) != C_TcpSocketSuccess) {
                add_counter(&server->total_errors, 1);
            }
            continue;
        }
        session->server = server;
        session->socket = accepted;
        session->task.task = NULL;
        add_counter(&server->total_connections, 1);
        add_counter(&server->active_sessions, 1);
        C_IOResult spawned = galay_coro_spawn(&server->runtime, session_entry, session, NULL, &session->task);
        if (spawned.code != C_IOResultOk) {
            add_counter(&server->total_errors, 1);
            if (galay_kernel_tcp_socket_destroy(&session->socket) != C_TcpSocketSuccess) {
                add_counter(&server->total_errors, 1);
            }
            atomic_store(&session->active, 0);
            subtract_counter(&server->active_sessions, 1);
        }
    }
}

static void* stats_thread_main(void* arg)
{
    ServerState* server = (ServerState*)arg;
    unsigned long long last_requests = 0;
    unsigned long long last_bytes = 0;

    while (atomic_load(&server->running)) {
        unsigned int remaining = sleep(1);
        if (remaining != 0) {
            continue;
        }
        unsigned long long requests = atomic_load(&server->total_requests);
        unsigned long long bytes = atomic_load(&server->total_bytes);
        unsigned long long connections = atomic_load(&server->total_connections);
        unsigned long long errors = atomic_load(&server->total_errors);
        unsigned long long delta_requests = requests - last_requests;
        unsigned long long delta_bytes = bytes - last_bytes;

        if (printf("[Stats] Connections: %llu | Requests/s: %llu | Throughput: %.3f MB/s | Total Requests: %llu | Errors: %llu\n",
                   connections,
                   delta_requests,
                   (double)delta_bytes / 1024.0 / 1024.0,
                   requests,
                   errors) < 0 ||
            fflush(stdout) != 0) {
            add_counter(&server->total_errors, 1);
            atomic_store(&server->running, 0);
            break;
        }

        last_requests = requests;
        last_bytes = bytes;
    }
    return NULL;
}

static int wake_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    int status = 0;
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
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
    for (int i = 0; i < TCP_SERVER_MAX_SESSIONS; ++i) {
        atomic_init(&server.sessions[i].active, 0);
    }
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

    if (printf("C TCP benchmark server starting on port %u\n", server.port) < 0 ||
        printf("meta: role=server io_mode=plain scenario=tcp-echo io_schedulers=%zu compute_schedulers=0 mode=coro-direct\n",
               io_schedulers) < 0 ||
        fflush(stdout) != 0) {
        return 1;
    }

    if (galay_kernel_runtime_create(&config, &server.runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&server.runtime) != C_RuntimeSuccess ||
        galay_kernel_tcp_socket_create(&server.listener, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_bind(&server.listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&server.listener, 1024) != C_TcpSocketSuccess) {
        exit_code = 2;
        goto cleanup;
    }

    C_IOResult accept_spawned = galay_coro_spawn(&server.runtime, accept_entry, &server, NULL, &server.accept_task);
    if (accept_spawned.code != C_IOResultOk) {
        exit_code = 3;
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

    if (wake_listener(server.port) != 0 && exit_code == 0) {
        exit_code = 4;
    }
    C_IOResult accept_join = galay_coro_join(&server.accept_task, 3000);
    C_IOResult accept_destroy = galay_coro_destroy(&server.accept_task);
    if ((accept_join.code != C_IOResultOk || accept_destroy.code != C_IOResultOk) && exit_code == 0) {
        exit_code = 5;
    }

    for (int i = 0; i < TCP_SERVER_MAX_SESSIONS; ++i) {
        if (server.sessions[i].task.task != NULL) {
            C_IOResult joined = galay_coro_join(&server.sessions[i].task, 3000);
            C_IOResult destroyed = galay_coro_destroy(&server.sessions[i].task);
            if ((joined.code != C_IOResultOk || destroyed.code != C_IOResultOk) && exit_code == 0) {
                exit_code = 6;
            }
        }
    }

    if (stats_started) {
        if (pthread_join(stats_thread, NULL) != 0 && exit_code == 0) {
            exit_code = 7;
        }
        stats_started = 0;
    }

cleanup:
    atomic_store(&server.running, 0);
    if (stats_started && pthread_join(stats_thread, NULL) != 0 && exit_code == 0) {
        exit_code = 8;
    }
    if (server.accept_task.task != NULL) {
        if (wake_listener(server.port) != 0 && exit_code == 0) {
            exit_code = 9;
        }
        if (galay_coro_join(&server.accept_task, 3000).code == C_IOResultOk) {
            if (galay_coro_destroy(&server.accept_task).code != C_IOResultOk && exit_code == 0) {
                exit_code = 10;
            }
        } else if (exit_code == 0) {
            exit_code = 11;
        }
    }
    if (server.listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&server.listener) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 12;
    }
    if (server.runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&server.runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 13;
    }
    if (server.runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&server.runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 14;
    }

    if (printf("tcp_socket_server_throughput connections=%llu requests=%llu throughput_mb=%.3f errors=%llu\n",
               atomic_load(&server.total_connections),
               atomic_load(&server.total_requests),
               (double)atomic_load(&server.total_bytes) / 1024.0 / 1024.0,
               atomic_load(&server.total_errors)) < 0 &&
        exit_code == 0) {
        exit_code = 15;
    }
    return exit_code;
}
