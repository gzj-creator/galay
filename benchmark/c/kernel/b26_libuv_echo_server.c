/**
 * @file b26_libuv_echo_server.c
 * @brief 与 Galay TCP/UDP benchmark 使用相同 echo wire 的 libuv 服务端基线。
 *
 * 单事件循环运行，TCP 每连接仅允许一个在途写，UDP 优先使用 try_send，避免
 * benchmark 自身引入额外线程或队列。该程序只用于本机同后端竞品对照。
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GALAY_BENCHMARK_HAS_LIBUV
#include <uv.h>

enum { kBufferBytes = 65536, kBacklog = 4096 };

typedef struct TcpClientState {
    uv_tcp_t handle;
    uv_write_t write_request;
    char buffer[kBufferBytes];
} TcpClientState;

typedef struct UdpSendState {
    uv_udp_send_t request;
    char* buffer;
} UdpSendState;

typedef struct UdpServerState {
    uv_udp_t handle;
    char buffer[kBufferBytes];
} UdpServerState;

static uv_loop_t* g_loop;
static uv_timer_t g_reporter;
static uv_signal_t g_sigint;
static uv_signal_t g_sigterm;
static uint64_t g_requests;
static uint64_t g_bytes;
static uint64_t g_connections;
static uint64_t g_errors;
static uint64_t g_last_requests;
static uint64_t g_last_bytes;

static void on_tcp_read(uv_stream_t* stream, ssize_t bytes, const uv_buf_t* buffer);

static const char* benchmark_backend(void)
{
#if defined(__APPLE__)
    return "kqueue";
#elif defined(__linux__)
    return "epoll";
#else
    return "unknown";
#endif
}

static void close_client(uv_handle_t* handle)
{
    free(handle->data);
}

static void alloc_tcp_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buffer)
{
    TcpClientState* client = (TcpClientState*)handle->data;
    (void)suggested_size;
    *buffer = uv_buf_init(client->buffer, (unsigned int)sizeof(client->buffer));
}

static void on_tcp_write(uv_write_t* request, int status)
{
    TcpClientState* client = (TcpClientState*)request->data;
    if (status < 0) {
        ++g_errors;
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }
    if (uv_read_start((uv_stream_t*)&client->handle, alloc_tcp_buffer, on_tcp_read) != 0) {
        ++g_errors;
        uv_close((uv_handle_t*)&client->handle, close_client);
    }
}

static void on_tcp_read(uv_stream_t* stream, ssize_t bytes, const uv_buf_t* buffer)
{
    TcpClientState* client = (TcpClientState*)stream->data;
    (void)buffer;
    if (bytes <= 0) {
        if (bytes < 0 && bytes != UV_EOF) {
            ++g_errors;
        }
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }

    if (uv_read_stop(stream) != 0) {
        ++g_errors;
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }
    uv_buf_t echo = uv_buf_init(client->buffer, (unsigned int)bytes);
    client->write_request.data = client;
    const int result = uv_write(&client->write_request, stream, &echo, 1, on_tcp_write);
    if (result != 0) {
        ++g_errors;
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }
    ++g_requests;
    g_bytes += (uint64_t)bytes * 2u;
}

static void on_tcp_connection(uv_stream_t* server, int status)
{
    if (status < 0) {
        ++g_errors;
        return;
    }

    TcpClientState* client = (TcpClientState*)calloc(1, sizeof(*client));
    if (client == NULL || uv_tcp_init(g_loop, &client->handle) != 0) {
        free(client);
        ++g_errors;
        return;
    }
    client->handle.data = client;
    if (uv_accept(server, (uv_stream_t*)&client->handle) != 0 ||
        uv_read_start((uv_stream_t*)&client->handle, alloc_tcp_buffer, on_tcp_read) != 0) {
        ++g_errors;
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }
    ++g_connections;
}

static void free_udp_send(uv_udp_send_t* request, int status)
{
    UdpSendState* state = (UdpSendState*)request->data;
    if (status < 0) {
        ++g_errors;
    }
    free(state->buffer);
    free(state);
}

static void alloc_udp_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buffer)
{
    UdpServerState* server = (UdpServerState*)handle->data;
    (void)suggested_size;
    *buffer = uv_buf_init(server->buffer, (unsigned int)sizeof(server->buffer));
}

static void on_udp_read(uv_udp_t* socket,
                        ssize_t bytes,
                        const uv_buf_t* buffer,
                        const struct sockaddr* peer,
                        unsigned int flags)
{
    (void)flags;
    if (bytes <= 0 || peer == NULL) {
        if (bytes < 0) {
            ++g_errors;
        }
        return;
    }

    uv_buf_t echo = uv_buf_init(buffer->base, (unsigned int)bytes);
    const int sent = uv_udp_try_send(socket, &echo, 1, peer);
    if (sent == bytes) {
        ++g_requests;
        g_bytes += (uint64_t)bytes * 2u;
        return;
    }

    UdpSendState* state = (UdpSendState*)calloc(1, sizeof(*state));
    if (state == NULL) {
        ++g_errors;
        return;
    }
    state->buffer = (char*)malloc((size_t)bytes);
    if (state->buffer == NULL) {
        ++g_errors;
        free(state);
        return;
    }
    if (memcpy(state->buffer, buffer->base, (size_t)bytes) != state->buffer) {
        ++g_errors;
        free(state->buffer);
        free(state);
        return;
    }
    echo = uv_buf_init(state->buffer, (unsigned int)bytes);
    state->request.data = state;
    if (uv_udp_send(&state->request, socket, &echo, 1, peer, free_udp_send) != 0) {
        ++g_errors;
        free(state->buffer);
        free(state);
        return;
    }
    ++g_requests;
    g_bytes += (uint64_t)bytes * 2u;
}

static void report_stats(uv_timer_t* timer)
{
    (void)timer;
    const uint64_t request_delta = g_requests - g_last_requests;
    const uint64_t byte_delta = g_bytes - g_last_bytes;
    if (printf("requests_per_sec=%llu throughput_mb_per_sec=%.3f total_requests=%llu errors=%llu\n",
               (unsigned long long)request_delta,
               (double)byte_delta / 1024.0 / 1024.0,
               (unsigned long long)g_requests,
               (unsigned long long)g_errors) < 0 ||
        fflush(stdout) != 0) {
        ++g_errors;
    }
    g_last_requests = g_requests;
    g_last_bytes = g_bytes;
}

static void stop_loop(uv_signal_t* signal_handle, int signum)
{
    (void)signal_handle;
    (void)signum;
    uv_stop(g_loop);
}

static int parse_port(const char* text, int* port)
{
    char* end = NULL;
    const long parsed = strtol(text, &end, 10);
    if (text[0] == '\0' || end == NULL || *end != '\0' || parsed < 1 || parsed > 65535) {
        return 1;
    }
    *port = (int)parsed;
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 3 || (strcmp(argv[1], "tcp") != 0 && strcmp(argv[1], "udp") != 0)) {
        if (fprintf(stderr, "Usage: %s <tcp|udp> <port>\n", argv[0]) < 0) {
            return 5;
        }
        return 1;
    }
    int port = 0;
    if (parse_port(argv[2], &port) != 0) {
        return 1;
    }

    uv_loop_t loop;
    if (uv_loop_init(&loop) != 0) {
        return 2;
    }
    g_loop = &loop;

    struct sockaddr_in address;
    if (uv_ip4_addr("127.0.0.1", port, &address) != 0 ||
        uv_timer_init(&loop, &g_reporter) != 0 ||
        uv_timer_start(&g_reporter, report_stats, 1000, 1000) != 0 ||
        uv_signal_init(&loop, &g_sigint) != 0 ||
        uv_signal_start(&g_sigint, stop_loop, SIGINT) != 0 ||
        uv_signal_init(&loop, &g_sigterm) != 0 ||
        uv_signal_start(&g_sigterm, stop_loop, SIGTERM) != 0) {
        const int close_result = uv_loop_close(&loop);
        if (close_result != 0 && close_result != UV_EBUSY) {
            return 5;
        }
        return 3;
    }

    int result = 0;
    if (strcmp(argv[1], "tcp") == 0) {
        uv_tcp_t server;
        result = uv_tcp_init(&loop, &server);
        if (result == 0) {
            result = uv_tcp_bind(&server, (const struct sockaddr*)&address, 0);
        }
        if (result == 0) {
            result = uv_listen((uv_stream_t*)&server, kBacklog, on_tcp_connection);
        }
        if (result == 0) {
            if (printf("meta: implementation=libuv version=%s backend=%s role=server scenario=tcp-echo port=%d loops=1\n",
                       uv_version_string(), benchmark_backend(), port) < 0) {
                result = UV_EIO;
            } else {
                const int active_handles = uv_run(&loop, UV_RUN_DEFAULT);
                if (printf("shutdown_active_handles=%d errors=%llu\n",
                           active_handles,
                           (unsigned long long)g_errors) < 0) {
                    result = UV_EIO;
                }
            }
        }
    } else {
        UdpServerState server = {0};
        result = uv_udp_init(&loop, &server.handle);
        if (result == 0) {
            server.handle.data = &server;
            result = uv_udp_bind(&server.handle, (const struct sockaddr*)&address, 0);
        }
        if (result == 0) {
            result = uv_udp_recv_start(&server.handle, alloc_udp_buffer, on_udp_read);
        }
        if (result == 0) {
            if (printf("meta: implementation=libuv version=%s backend=%s role=server scenario=udp-echo port=%d loops=1\n",
                       uv_version_string(), benchmark_backend(), port) < 0) {
                result = UV_EIO;
            } else {
                const int active_handles = uv_run(&loop, UV_RUN_DEFAULT);
                if (printf("shutdown_active_handles=%d errors=%llu\n",
                           active_handles,
                           (unsigned long long)g_errors) < 0) {
                    result = UV_EIO;
                }
            }
        }
    }

    if (result != 0) {
        if (fprintf(stderr, "libuv error: %s\n", uv_strerror(result)) < 0) {
            return 5;
        }
        return 4;
    }
    const int close_result = uv_loop_close(&loop);
    if (close_result != 0 && close_result != UV_EBUSY) {
        return 5;
    }
    return 0;
}

#else

int main(void)
{
    if (fprintf(stderr,
                "libuv competitor unavailable: pkg-config module libuv was not found at configure time\n") < 0) {
        return 5;
    }
    return 77;
}

#endif
