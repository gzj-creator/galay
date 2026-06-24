#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

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
typedef struct ClientSession ClientSession;

struct ClientState {
    galay_kernel_runtime_t runtime;
    ClientConfig config;
    atomic_int start;
    atomic_int stop;
    atomic_int ready_count;
    atomic_int done_count;
};

struct ClientSession {
    ClientState* state;
    galay_kernel_tcp_socket_t socket;
    char* request;
    char* response;
    char* recv_buffer;
    size_t send_offset;
    size_t recv_offset;
    uint64_t requests;
    uint64_t bytes;
    uint64_t errors;
    atomic_int connected;
    atomic_int started;
    atomic_int done;
    int closing;
};

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void start_session(ClientSession* session);
static void post_send(ClientSession* session);
static void start_recv_loop(ClientSession* session);
static void close_session(ClientSession* session);

static void on_close(C_TcpSocketResultCode code, void* ctx)
{
    ClientSession* session = (ClientSession*)ctx;
    if (code != C_TcpSocketSuccess) {
        ++session->errors;
    }
    (void)galay_kernel_tcp_socket_destroy(&session->socket);
    atomic_store(&session->done, 1);
    atomic_fetch_add(&session->state->done_count, 1);
}

static void close_session(ClientSession* session)
{
    if (session == 0 || session->closing) {
        return;
    }
    session->closing = 1;
    if (session->socket.socket != 0 &&
        galay_kernel_tcp_socket_close(&session->state->runtime, &session->socket, on_close, session) == C_TcpSocketSuccess) {
        return;
    }
    if (session->socket.socket != 0) {
        (void)galay_kernel_tcp_socket_destroy(&session->socket);
    }
    atomic_store(&session->done, 1);
    atomic_fetch_add(&session->state->done_count, 1);
}

static int on_recv(galay_kernel_tcp_recv_result_t* result, void* ctx)
{
    ClientSession* session = (ClientSession*)ctx;
    if (session->closing) {
        return 1;
    }
    if (result == 0 || result->code != C_TcpSocketSuccess || result->bytes == 0) {
        ++session->errors;
        close_session(session);
        return 1;
    }

    if (result->buffer != session->recv_buffer ||
        session->recv_offset + result->bytes > session->state->config.payload_bytes) {
        ++session->errors;
        close_session(session);
        return 1;
    }

    memcpy(session->response + session->recv_offset, result->buffer, result->bytes);
    session->recv_offset += result->bytes;
    if (session->recv_offset < session->state->config.payload_bytes) {
        return 0;
    }

    if (memcmp(session->request, session->response, session->state->config.payload_bytes) != 0) {
        ++session->errors;
        close_session(session);
        return 1;
    }

    ++session->requests;
    session->bytes += (uint64_t)session->state->config.payload_bytes * 2u;
    if (atomic_load(&session->state->stop)) {
        close_session(session);
        return 1;
    }

    session->send_offset = 0;
    session->recv_offset = 0;
    memset(session->response, 0, session->state->config.payload_bytes);
    post_send(session);
    return 0;
}

static void start_recv_loop(ClientSession* session)
{
    if (galay_kernel_tcp_socket_recv_loop(
            &session->state->runtime,
            &session->socket,
            session->recv_buffer,
            session->state->config.payload_bytes,
            on_recv,
            session) != C_TcpSocketSuccess) {
        ++session->errors;
        close_session(session);
    }
}

static void on_send(galay_kernel_tcp_send_result_t* result, void* ctx)
{
    ClientSession* session = (ClientSession*)ctx;
    if (result == 0 || result->code != C_TcpSocketSuccess || result->bytes == 0) {
        ++session->errors;
        close_session(session);
        return;
    }

    session->send_offset += result->bytes;
    if (session->send_offset < session->state->config.payload_bytes) {
        post_send(session);
        return;
    }

    memset(session->response, 0, session->state->config.payload_bytes);
    session->recv_offset = 0;
}

static void post_send(ClientSession* session)
{
    if (atomic_load(&session->state->stop)) {
        close_session(session);
        return;
    }

    const size_t remaining = session->state->config.payload_bytes - session->send_offset;
    if (galay_kernel_tcp_socket_send(
            &session->state->runtime,
            &session->socket,
            session->request + session->send_offset,
            remaining,
            on_send,
            session) != C_TcpSocketSuccess) {
        ++session->errors;
        close_session(session);
    }
}

static void start_session(ClientSession* session)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&session->started, &expected, 1)) {
        return;
    }
    session->send_offset = 0;
    session->recv_offset = 0;
    memset(session->response, 0, session->state->config.payload_bytes);
    start_recv_loop(session);
    if (session->closing) {
        return;
    }
    post_send(session);
}

static void on_connect(C_TcpSocketResultCode code, void* ctx)
{
    ClientSession* session = (ClientSession*)ctx;
    if (code != C_TcpSocketSuccess) {
        ++session->errors;
        close_session(session);
        return;
    }

    atomic_store(&session->connected, 1);
    atomic_fetch_add(&session->state->ready_count, 1);
    if (atomic_load(&session->state->start)) {
        start_session(session);
    }
}

static void init_request(ClientSession* session)
{
    for (size_t i = 0; i < session->state->config.payload_bytes; ++i) {
        session->request[i] = (char)('a' + (i % 26));
    }
}

static int connect_session(ClientSession* session)
{
    C_Host host = {C_IPTypeIPV4, {0}, session->state->config.port};
    strncpy(host.address, session->state->config.host, sizeof(host.address) - 1);

    if (galay_kernel_tcp_socket_create(&session->socket, C_IPTypeIPV4) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_connect(&session->state->runtime, &session->socket, &host, on_connect, session) != C_TcpSocketSuccess) {
        ++session->errors;
        close_session(session);
        return 1;
    }
    return 0;
}

static void print_usage(const char* program)
{
    printf("Usage: %s [-h host] [-p port] [-c connections] [-s payload_bytes] [-d duration_seconds] [--io-schedulers count]\n", program);
}

static int parse_args(int argc, char** argv, ClientConfig* config)
{
    memset(config, 0, sizeof(*config));
    strncpy(config->host, "127.0.0.1", sizeof(config->host) - 1);
    config->port = TCP_CLIENT_DEFAULT_PORT;
    config->connections = TCP_CLIENT_DEFAULT_CONNECTIONS;
    config->payload_bytes = TCP_CLIENT_DEFAULT_PAYLOAD_BYTES;
    config->duration_seconds = TCP_CLIENT_DEFAULT_DURATION_SECONDS;
    config->io_schedulers = TCP_CLIENT_DEFAULT_IO_SCHEDULERS;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            strncpy(config->host, argv[++i], sizeof(config->host) - 1);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            config->port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config->connections = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
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

    return config->connections <= 0 ||
        config->payload_bytes == 0 ||
        config->duration_seconds <= 0 ||
        config->io_schedulers == 0;
}

int main(int argc, char** argv)
{
    ClientState state;
    memset(&state, 0, sizeof(state));
    if (parse_args(argc, argv, &state.config) != 0) {
        return 1;
    }

    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = state.config.io_schedulers;
    runtime_config.compute_scheduler_count = 0;

    if (galay_kernel_runtime_create(&runtime_config, &state.runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&state.runtime) != C_RuntimeSuccess) {
        return 2;
    }

    atomic_init(&state.start, 0);
    atomic_init(&state.stop, 0);
    atomic_init(&state.ready_count, 0);
    atomic_init(&state.done_count, 0);

    ClientSession* sessions = (ClientSession*)calloc((size_t)state.config.connections, sizeof(ClientSession));
    if (sessions == 0) {
        (void)galay_kernel_runtime_stop(&state.runtime);
        (void)galay_kernel_runtime_destroy(&state.runtime);
        return 3;
    }

    for (int i = 0; i < state.config.connections; ++i) {
        sessions[i].state = &state;
        atomic_init(&sessions[i].connected, 0);
        atomic_init(&sessions[i].started, 0);
        atomic_init(&sessions[i].done, 0);
        sessions[i].request = (char*)malloc(state.config.payload_bytes);
        sessions[i].response = (char*)malloc(state.config.payload_bytes);
        sessions[i].recv_buffer = (char*)malloc(state.config.payload_bytes);
        if (sessions[i].request == 0 || sessions[i].response == 0 || sessions[i].recv_buffer == 0) {
            ++sessions[i].errors;
            close_session(&sessions[i]);
            continue;
        }
        init_request(&sessions[i]);
        (void)connect_session(&sessions[i]);
    }

    const int64_t connect_deadline = now_us() + 10000000;
    while (atomic_load(&state.ready_count) < state.config.connections && now_us() < connect_deadline) {
        usleep(1000);
    }

    const int64_t start_us = now_us();
    atomic_store(&state.start, 1);
    for (int i = 0; i < state.config.connections; ++i) {
        if (atomic_load(&sessions[i].connected)) {
            start_session(&sessions[i]);
        }
    }

    usleep((useconds_t)state.config.duration_seconds * 1000000u);
    atomic_store(&state.stop, 1);

    const int64_t close_deadline = now_us() + 3000000;
    while (atomic_load(&state.done_count) < state.config.connections && now_us() < close_deadline) {
        usleep(1000);
    }

    const int64_t elapsed_us = now_us() - start_us;
    uint64_t total_requests = 0;
    uint64_t total_bytes = 0;
    uint64_t total_errors = 0;
    for (int i = 0; i < state.config.connections; ++i) {
        total_requests += sessions[i].requests;
        total_bytes += sessions[i].bytes;
        total_errors += sessions[i].errors;
        free(sessions[i].request);
        free(sessions[i].response);
        free(sessions[i].recv_buffer);
    }

    const double seconds = elapsed_us > 0 ? (double)elapsed_us / 1000000.0 : 0.0;
    const double qps = seconds > 0.0 ? (double)total_requests / seconds : 0.0;
    const double throughput = seconds > 0.0 ? (double)total_bytes / seconds / 1024.0 / 1024.0 : 0.0;

    printf("tcp_socket_client_throughput io_schedulers=%zu compute_schedulers=0 connections=%d duration_sec=%d payload_bytes=%zu elapsed_ms=%.3f requests=%llu qps=%.2f throughput_mb_per_sec=%.3f errors=%llu mode=callback-loop\n",
           state.config.io_schedulers,
           state.config.connections,
           state.config.duration_seconds,
           state.config.payload_bytes,
           (double)elapsed_us / 1000.0,
           (unsigned long long)total_requests,
           qps,
           throughput,
           (unsigned long long)total_errors);

    free(sessions);
    (void)galay_kernel_runtime_stop(&state.runtime);
    (void)galay_kernel_runtime_destroy(&state.runtime);
    return total_errors == 0 ? 0 : 4;
}
