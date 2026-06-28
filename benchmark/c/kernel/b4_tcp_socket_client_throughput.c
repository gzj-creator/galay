#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

enum {
    TCP_CLIENT_DEFAULT_PORT = 19080,
    TCP_CLIENT_DEFAULT_CONNECTIONS = 32,
    TCP_CLIENT_DEFAULT_PAYLOAD_BYTES = 1024,
    TCP_CLIENT_DEFAULT_DURATION_SECONDS = 5,
    TCP_CLIENT_DEFAULT_IO_SCHEDULERS = 1
};

typedef struct ClientConfig {
    char host[64];
    uint16_t port;
    int connections;
    size_t payload_bytes;
    int duration_seconds;
    size_t io_schedulers;
} ClientConfig;

typedef struct ClientState ClientState;

typedef struct ClientSession {
    ClientState* state;
    galay_kernel_tcp_socket_t socket;
    galay_coro_task_t task;
    char* request;
    char* response;
    uint64_t requests;
    uint64_t bytes;
    uint64_t errors;
    atomic_int done;
} ClientSession;

struct ClientState {
    galay_kernel_runtime_t runtime;
    ClientConfig config;
    atomic_int stop;
    atomic_int done_count;
};

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
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

static int recv_all(galay_kernel_tcp_socket_t* socket, char* buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        C_IOResult result = galay_kernel_tcp_socket_recv(socket, buffer + received, length - received, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        received += result.bytes;
    }
    return 0;
}

static void session_entry(void* arg)
{
    ClientSession* session = (ClientSession*)arg;
    C_Host host = {C_IPTypeIPV4, "", session->state->config.port};
    int copied = snprintf(host.address, sizeof(host.address), "%s", session->state->config.host);
    if (copied < 0 || (size_t)copied >= sizeof(host.address)) {
        ++session->errors;
        goto done;
    }

    if (galay_kernel_tcp_socket_create(&session->socket, C_IPTypeIPV4) != C_TcpSocketSuccess) {
        ++session->errors;
        goto done;
    }

    C_IOResult connected = galay_kernel_tcp_socket_connect(&session->socket, &host, 3000);
    if (connected.code != C_IOResultOk) {
        ++session->errors;
        goto done;
    }

    while (!atomic_load(&session->state->stop)) {
        if (send_all(&session->socket, session->request, session->state->config.payload_bytes) != 0 ||
            recv_all(&session->socket, session->response, session->state->config.payload_bytes) != 0 ||
            memcmp(session->request, session->response, session->state->config.payload_bytes) != 0) {
            ++session->errors;
            break;
        }
        ++session->requests;
        session->bytes += (uint64_t)session->state->config.payload_bytes * 2u;
    }

    {
        C_IOResult closed = galay_kernel_tcp_socket_close(&session->socket, 1000);
        if (closed.code != C_IOResultOk && closed.code != C_IOResultEof) {
            ++session->errors;
        }
    }

done:
    atomic_store(&session->done, 1);
    int previous_done_count = atomic_fetch_add(&session->state->done_count, 1);
    if (previous_done_count < 0) {
        ++session->errors;
    }
}

static void init_request(ClientSession* session)
{
    for (size_t i = 0; i < session->state->config.payload_bytes; ++i) {
        session->request[i] = (char)('a' + (i % 26));
    }
}

static int print_usage(const char* program)
{
    return printf("Usage: %s [-h host] [-p port] [-c connections] [-s payload_bytes] [-d duration_seconds] [--io-schedulers count]\n",
                  program) < 0
        ? 1
        : 0;
}

static int parse_args(int argc, char** argv, ClientConfig* config)
{
    *config = (ClientConfig){0};
    int copied = snprintf(config->host, sizeof(config->host), "%s", "127.0.0.1");
    if (copied < 0 || (size_t)copied >= sizeof(config->host)) {
        return 2;
    }
    config->port = TCP_CLIENT_DEFAULT_PORT;
    config->connections = TCP_CLIENT_DEFAULT_CONNECTIONS;
    config->payload_bytes = TCP_CLIENT_DEFAULT_PAYLOAD_BYTES;
    config->duration_seconds = TCP_CLIENT_DEFAULT_DURATION_SECONDS;
    config->io_schedulers = TCP_CLIENT_DEFAULT_IO_SCHEDULERS;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            copied = snprintf(config->host, sizeof(config->host), "%s", argv[++i]);
            if (copied < 0 || (size_t)copied >= sizeof(config->host)) {
                return 2;
            }
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            config->port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config->connections = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
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

    return config->connections <= 0 ||
        config->payload_bytes == 0 ||
        config->duration_seconds <= 0 ||
        config->io_schedulers == 0;
}

int main(int argc, char** argv)
{
    ClientState state = {0};
    if (parse_args(argc, argv, &state.config) != 0) {
        return 1;
    }

    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = state.config.io_schedulers;
    runtime_config.compute_scheduler_count = 0;
    int exit_code = 0;

    if (galay_kernel_runtime_create(&runtime_config, &state.runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&state.runtime) != C_RuntimeSuccess) {
        return 2;
    }

    atomic_init(&state.stop, 0);
    atomic_init(&state.done_count, 0);

    ClientSession* sessions = (ClientSession*)calloc((size_t)state.config.connections, sizeof(ClientSession));
    if (sessions == NULL) {
        exit_code = 3;
        goto cleanup_runtime;
    }

    for (int i = 0; i < state.config.connections; ++i) {
        sessions[i].state = &state;
        atomic_init(&sessions[i].done, 0);
        sessions[i].request = (char*)malloc(state.config.payload_bytes);
        sessions[i].response = (char*)malloc(state.config.payload_bytes);
        if (sessions[i].request == NULL || sessions[i].response == NULL) {
            ++sessions[i].errors;
            atomic_store(&sessions[i].done, 1);
            int previous_done_count = atomic_fetch_add(&state.done_count, 1);
            if (previous_done_count < 0) {
                ++sessions[i].errors;
            }
            continue;
        }
        init_request(&sessions[i]);
    }

    const int64_t start_us = now_us();
    for (int i = 0; i < state.config.connections; ++i) {
        if (!atomic_load(&sessions[i].done)) {
            C_IOResult spawn_result = galay_coro_spawn(
                &state.runtime,
                session_entry,
                &sessions[i],
                NULL,
                &sessions[i].task);
            if (spawn_result.code != C_IOResultOk) {
                ++sessions[i].errors;
                atomic_store(&sessions[i].done, 1);
                int previous_done_count = atomic_fetch_add(&state.done_count, 1);
                if (previous_done_count < 0) {
                    ++sessions[i].errors;
                }
            }
        }
    }

    if (usleep((useconds_t)state.config.duration_seconds * 1000000u) != 0) {
        exit_code = 4;
    }
    atomic_store(&state.stop, 1);

    const int64_t close_deadline = now_us() + 3000000;
    while (atomic_load(&state.done_count) < state.config.connections && now_us() < close_deadline) {
        if (usleep(1000) != 0) {
            exit_code = 5;
            break;
        }
    }

    const int64_t elapsed_us = now_us() - start_us;
    uint64_t total_requests = 0;
    uint64_t total_bytes = 0;
    uint64_t total_errors = 0;
    for (int i = 0; i < state.config.connections; ++i) {
        if (!atomic_load(&sessions[i].done)) {
            ++sessions[i].errors;
            if (exit_code == 0) {
                exit_code = 6;
            }
        }
        if (sessions[i].task.task != NULL) {
            C_IOResult join_result = galay_coro_join(&sessions[i].task, 3000);
            C_IOResult destroy_result = galay_coro_destroy(&sessions[i].task);
            if ((join_result.code != C_IOResultOk || destroy_result.code != C_IOResultOk) && exit_code == 0) {
                exit_code = 7;
            }
            if (destroy_result.code == C_IOResultOk) {
                sessions[i].task.task = NULL;
            }
        }
        total_requests += sessions[i].requests;
        total_bytes += sessions[i].bytes;
        total_errors += sessions[i].errors;
        if (sessions[i].socket.socket != NULL &&
            galay_kernel_tcp_socket_destroy(&sessions[i].socket) != C_TcpSocketSuccess &&
            exit_code == 0) {
            exit_code = 8;
        }
        free(sessions[i].request);
        free(sessions[i].response);
        sessions[i].request = NULL;
        sessions[i].response = NULL;
    }

    const double seconds = elapsed_us > 0 ? (double)elapsed_us / 1000000.0 : 0.0;
    const double qps = seconds > 0.0 ? (double)total_requests / seconds : 0.0;
    const double throughput = seconds > 0.0 ? (double)total_bytes / seconds / 1024.0 / 1024.0 : 0.0;

    if (printf("tcp_socket_client_throughput io_schedulers=%zu compute_schedulers=0 connections=%d duration_sec=%d payload_bytes=%zu elapsed_ms=%.3f requests=%llu qps=%.2f throughput_mb_per_sec=%.3f errors=%llu mode=coro-direct\n",
               state.config.io_schedulers,
               state.config.connections,
               state.config.duration_seconds,
               state.config.payload_bytes,
               (double)elapsed_us / 1000.0,
               (unsigned long long)total_requests,
               qps,
               throughput,
               (unsigned long long)total_errors) < 0 &&
        exit_code == 0) {
        exit_code = 9;
    }
    if (total_errors != 0 && exit_code == 0) {
        exit_code = 10;
    }

    for (int i = 0; i < state.config.connections; ++i) {
        if (sessions[i].task.task != NULL) {
            if (galay_coro_join(&sessions[i].task, 0).code == C_IOResultOk) {
                if (galay_coro_destroy(&sessions[i].task).code != C_IOResultOk && exit_code == 0) {
                    exit_code = 11;
                }
            }
        }
        if (sessions[i].socket.socket != NULL &&
            galay_kernel_tcp_socket_destroy(&sessions[i].socket) != C_TcpSocketSuccess &&
            exit_code == 0) {
            exit_code = 12;
        }
        free(sessions[i].request);
        free(sessions[i].response);
    }
    free(sessions);

cleanup_runtime:
    if (state.runtime.runtime != NULL &&
        galay_kernel_runtime_stop(&state.runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 13;
    }
    if (state.runtime.runtime != NULL &&
        galay_kernel_runtime_destroy(&state.runtime) != C_RuntimeSuccess &&
        exit_code == 0) {
        exit_code = 14;
    }
    return exit_code;
}
