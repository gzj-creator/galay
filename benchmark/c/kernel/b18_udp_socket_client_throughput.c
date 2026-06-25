#include <galay/c/galay-kernel-c/async-c/udp_socket_c.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
    UDP_CLIENT_DEFAULT_PORT = 9090,
    UDP_CLIENT_DEFAULT_CLIENTS = 100,
    UDP_CLIENT_DEFAULT_MESSAGES = 1000,
    UDP_CLIENT_DEFAULT_MESSAGE_BYTES = 256,
    UDP_CLIENT_DEFAULT_DURATION_SECONDS = 5,
    UDP_CLIENT_DEFAULT_IO_SCHEDULERS = 1,
    UDP_CLIENT_RECV_TIMEOUT_MS = 50
};

typedef struct ClientConfig {
    char host[64];
    uint16_t port;
    int clients;
    int messages_per_client;
    size_t message_bytes;
    int duration_seconds;
    size_t io_schedulers;
} ClientConfig;

typedef struct ClientState ClientState;
typedef struct ClientSession ClientSession;

struct ClientState {
    galay_kernel_runtime_t runtime;
    ClientConfig config;
    C_Host server_host;
    atomic_int stop;
    atomic_int done_count;
};

struct ClientSession {
    ClientState* state;
    galay_kernel_udp_socket_t socket;
    char* message;
    char* recv_buffer;
    int sent_messages;
    uint64_t sent;
    uint64_t received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t errors;
    atomic_int closing;
    atomic_int done;
};

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

static void post_send(ClientSession* session);
static void post_recv(ClientSession* session);
static void close_session(ClientSession* session);

static void on_close(C_UdpSocketResultCode code, void* ctx)
{
    ClientSession* session = (ClientSession*)ctx;
    if (code != C_UdpSocketSuccess) {
        ++session->errors;
    }
    atomic_store(&session->done, 1);
    atomic_fetch_add(&session->state->done_count, 1);
}

static void close_session(ClientSession* session)
{
    int expected = 0;
    if (session == 0 || !atomic_compare_exchange_strong(&session->closing, &expected, 1)) {
        return;
    }

    if (session->socket.socket != 0 &&
        galay_kernel_udp_socket_close(&session->state->runtime, &session->socket, on_close, session) ==
            C_UdpSocketSuccess) {
        return;
    }
    ++session->errors;
    atomic_store(&session->done, 1);
    atomic_fetch_add(&session->state->done_count, 1);
}

static int should_stop_session(ClientSession* session)
{
    return atomic_load(&session->state->stop) ||
        session->sent_messages >= session->state->config.messages_per_client;
}

static void on_recv(galay_kernel_udp_recvfrom_result_t* result, void* ctx)
{
    ClientSession* session = (ClientSession*)ctx;
    if (atomic_load(&session->closing)) {
        return;
    }

    if (result == 0) {
        ++session->errors;
        close_session(session);
        return;
    }

    if (result->code == C_UdpSocketSuccess) {
        if (result->bytes != session->state->config.message_bytes ||
            memcmp(session->recv_buffer, session->message, session->state->config.message_bytes) != 0) {
            ++session->errors;
            close_session(session);
            return;
        }
        ++session->received;
        session->bytes_received += result->bytes;
    } else if (result->code != C_UdpSocketTimeout) {
        ++session->errors;
        close_session(session);
        return;
    }

    if (should_stop_session(session)) {
        close_session(session);
        return;
    }
    post_send(session);
}

static void post_recv(ClientSession* session)
{
    if (galay_kernel_udp_socket_recvfrom_timeout(
            &session->state->runtime,
            &session->socket,
            session->recv_buffer,
            session->state->config.message_bytes,
            UDP_CLIENT_RECV_TIMEOUT_MS,
            on_recv,
            session) != C_UdpSocketSuccess) {
        ++session->errors;
        close_session(session);
    }
}

static void on_send(galay_kernel_udp_sendto_result_t* result, void* ctx)
{
    ClientSession* session = (ClientSession*)ctx;
    if (atomic_load(&session->closing)) {
        return;
    }

    if (result == 0 || result->code != C_UdpSocketSuccess ||
        result->bytes != session->state->config.message_bytes) {
        ++session->errors;
        close_session(session);
        return;
    }

    ++session->sent;
    ++session->sent_messages;
    session->bytes_sent += result->bytes;
    if (atomic_load(&session->state->stop)) {
        close_session(session);
        return;
    }
    post_recv(session);
}

static void post_send(ClientSession* session)
{
    if (should_stop_session(session)) {
        close_session(session);
        return;
    }

    if (galay_kernel_udp_socket_sendto(
            &session->state->runtime,
            &session->socket,
            session->message,
            session->state->config.message_bytes,
            &session->state->server_host,
            on_send,
            session) != C_UdpSocketSuccess) {
        ++session->errors;
        close_session(session);
    }
}

static void init_message(ClientSession* session, int index)
{
    for (size_t i = 0; i < session->state->config.message_bytes; ++i) {
        session->message[i] = (char)('a' + ((i + (size_t)index) % 26));
    }
}

static void print_usage(const char* program)
{
    printf("Usage: %s [-h host] [-p port] [-c clients] [-m messages] [-s size] [-d duration] [--io-schedulers count]\n",
           program);
}

static int parse_args(int argc, char** argv, ClientConfig* config)
{
    memset(config, 0, sizeof(*config));
    strncpy(config->host, "127.0.0.1", sizeof(config->host) - 1);
    config->port = UDP_CLIENT_DEFAULT_PORT;
    config->clients = UDP_CLIENT_DEFAULT_CLIENTS;
    config->messages_per_client = UDP_CLIENT_DEFAULT_MESSAGES;
    config->message_bytes = UDP_CLIENT_DEFAULT_MESSAGE_BYTES;
    config->duration_seconds = UDP_CLIENT_DEFAULT_DURATION_SECONDS;
    config->io_schedulers = UDP_CLIENT_DEFAULT_IO_SCHEDULERS;

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
            strncpy(config->host, argv[++i], sizeof(config->host) - 1);
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            config->port = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--clients") == 0) && i + 1 < argc) {
            config->clients = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--messages") == 0) && i + 1 < argc) {
            config->messages_per_client = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) && i + 1 < argc) {
            config->message_bytes = (size_t)strtoull(argv[++i], 0, 10);
        } else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0) && i + 1 < argc) {
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

    return config->clients <= 0 ||
        config->messages_per_client <= 0 ||
        config->message_bytes == 0 ||
        config->message_bytes > 65536 ||
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

    state.server_host.type = C_IPTypeIPV4;
    strncpy(state.server_host.address, state.config.host, sizeof(state.server_host.address) - 1);
    state.server_host.port = state.config.port;

    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = state.config.io_schedulers;
    runtime_config.compute_scheduler_count = 0;

    if (galay_kernel_runtime_create(&runtime_config, &state.runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&state.runtime) != C_RuntimeSuccess) {
        return 2;
    }

    atomic_init(&state.stop, 0);
    atomic_init(&state.done_count, 0);

    ClientSession* sessions = (ClientSession*)calloc((size_t)state.config.clients, sizeof(ClientSession));
    if (sessions == 0) {
        (void)galay_kernel_runtime_stop(&state.runtime);
        (void)galay_kernel_runtime_destroy(&state.runtime);
        return 3;
    }

    C_Host bind_host = {C_IPTypeIPV4, "0.0.0.0", 0};
    for (int i = 0; i < state.config.clients; ++i) {
        sessions[i].state = &state;
        atomic_init(&sessions[i].closing, 0);
        atomic_init(&sessions[i].done, 0);
        sessions[i].message = (char*)malloc(state.config.message_bytes);
        sessions[i].recv_buffer = (char*)malloc(state.config.message_bytes);
        if (sessions[i].message == 0 || sessions[i].recv_buffer == 0 ||
            galay_kernel_udp_socket_create(&sessions[i].socket, C_IPTypeIPV4) != C_UdpSocketSuccess ||
            galay_kernel_udp_socket_bind(&sessions[i].socket, &bind_host) != C_UdpSocketSuccess) {
            ++sessions[i].errors;
            atomic_store(&sessions[i].done, 1);
            atomic_fetch_add(&state.done_count, 1);
            continue;
        }
        init_message(&sessions[i], i);
    }

    printf("C UDP benchmark client starting host=%s port=%u clients=%d messages_per_client=%d message_size=%zu duration=%d\n",
           state.config.host,
           state.config.port,
           state.config.clients,
           state.config.messages_per_client,
           state.config.message_bytes,
           state.config.duration_seconds);
    printf("meta: role=client io_mode=plain scenario=udp-echo io_schedulers=%zu compute_schedulers=0 mode=callback-pipeline recv_timeout_ms=%d\n",
           state.config.io_schedulers,
           UDP_CLIENT_RECV_TIMEOUT_MS);
    fflush(stdout);

    const int64_t start_us = now_us();
    for (int i = 0; i < state.config.clients; ++i) {
        if (!atomic_load(&sessions[i].done)) {
            post_send(&sessions[i]);
        }
    }

    usleep((useconds_t)state.config.duration_seconds * 1000000u);
    atomic_store(&state.stop, 1);

    const int64_t close_deadline = now_us() + 10000000;
    while (atomic_load(&state.done_count) < state.config.clients && now_us() < close_deadline) {
        usleep(1000);
    }
    const int64_t elapsed_us = now_us() - start_us;

    uint64_t total_sent = 0;
    uint64_t total_received = 0;
    uint64_t total_bytes_sent = 0;
    uint64_t total_bytes_received = 0;
    uint64_t total_errors = 0;
    for (int i = 0; i < state.config.clients; ++i) {
        if (!atomic_load(&sessions[i].done)) {
            ++total_errors;
            close_session(&sessions[i]);
        }
    }
    const int64_t destroy_deadline = now_us() + 3000000;
    while (atomic_load(&state.done_count) < state.config.clients && now_us() < destroy_deadline) {
        usleep(1000);
    }

    for (int i = 0; i < state.config.clients; ++i) {
        total_sent += sessions[i].sent;
        total_received += sessions[i].received;
        total_bytes_sent += sessions[i].bytes_sent;
        total_bytes_received += sessions[i].bytes_received;
        total_errors += sessions[i].errors;
        if (sessions[i].socket.socket != 0) {
            (void)galay_kernel_udp_socket_destroy(&sessions[i].socket);
        }
        free(sessions[i].message);
        free(sessions[i].recv_buffer);
    }

    const double seconds = elapsed_us > 0 ? (double)elapsed_us / 1000000.0 : 0.0;
    const double sent_mb_s = seconds > 0.0 ? (double)total_bytes_sent / seconds / 1024.0 / 1024.0 : 0.0;
    const double recv_mb_s = seconds > 0.0 ? (double)total_bytes_received / seconds / 1024.0 / 1024.0 : 0.0;
    const double loss_percent = total_sent == 0
        ? 100.0
        : (1.0 - (double)total_received / (double)total_sent) * 100.0;

    printf("udp_socket_client_throughput io_schedulers=%zu clients=%d messages_per_client=%d message_size=%zu duration_sec=%d elapsed_ms=%.3f sent=%llu received=%llu loss_percent=%.3f sent_mb_s=%.3f recv_mb_s=%.3f errors=%llu mode=callback-pipeline\n",
           state.config.io_schedulers,
           state.config.clients,
           state.config.messages_per_client,
           state.config.message_bytes,
           state.config.duration_seconds,
           (double)elapsed_us / 1000.0,
           (unsigned long long)total_sent,
           (unsigned long long)total_received,
           loss_percent,
           sent_mb_s,
           recv_mb_s,
           (unsigned long long)total_errors);

    free(sessions);
    (void)galay_kernel_runtime_stop(&state.runtime);
    (void)galay_kernel_runtime_destroy(&state.runtime);
    return total_errors == 0 && total_sent > 0 ? 0 : 4;
}
