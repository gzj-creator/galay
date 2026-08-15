/**
 * @file b26_libuv_echo_server.c
 * @brief 与 Galay TCP/UDP benchmark 使用相同 echo wire 的 libuv 服务端基线。
 *
 * 单事件循环运行，TCP 每连接仅允许一个在途写，UDP 优先使用 try_send，避免
 * benchmark 自身引入额外线程或队列。该程序只用于本机同后端竞品对照。
 */

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GALAY_BENCHMARK_HAS_LIBUV
#include <uv.h>

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

enum { kBufferBytes = 65536, kBacklog = 4096, kMaxLoops = 64 };

typedef struct TcpClientState {
    uv_tcp_t handle;
    uv_write_t write_request;
    char buffer[kBufferBytes];
} TcpClientState;

typedef struct TcpServerWorker {
    uv_loop_t loop;
    uv_tcp_t server;
    uv_async_t stopper;
    uv_thread_t thread;
    struct sockaddr_in address;
    atomic_int ready;
    int start_result;
    int active_handles;
} TcpServerWorker;

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
static TcpServerWorker* g_tcp_workers;
static int g_tcp_worker_count;
static atomic_ullong g_requests;
static atomic_ullong g_bytes;
static atomic_ullong g_connections;
static atomic_ullong g_errors;
static uint64_t g_last_requests;
static uint64_t g_last_bytes;

static void on_tcp_read(uv_stream_t* stream, ssize_t bytes, const uv_buf_t* buffer);

static void add_counter(atomic_ullong* counter, uint64_t value)
{
    (void)atomic_fetch_add_explicit(counter, value, memory_order_relaxed);
}

static uint64_t load_counter(const atomic_ullong* counter)
{
    return atomic_load_explicit(counter, memory_order_relaxed);
}

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
        add_counter(&g_errors, 1);
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }
    if (uv_read_start((uv_stream_t*)&client->handle, alloc_tcp_buffer, on_tcp_read) != 0) {
        add_counter(&g_errors, 1);
        uv_close((uv_handle_t*)&client->handle, close_client);
    }
}

static void on_tcp_read(uv_stream_t* stream, ssize_t bytes, const uv_buf_t* buffer)
{
    TcpClientState* client = (TcpClientState*)stream->data;
    (void)buffer;
    if (bytes <= 0) {
        if (bytes < 0 && bytes != UV_EOF) {
            add_counter(&g_errors, 1);
        }
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }

    if (uv_read_stop(stream) != 0) {
        add_counter(&g_errors, 1);
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }
    uv_buf_t echo = uv_buf_init(client->buffer, (unsigned int)bytes);
    client->write_request.data = client;
    const int result = uv_write(&client->write_request, stream, &echo, 1, on_tcp_write);
    if (result != 0) {
        add_counter(&g_errors, 1);
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }
    add_counter(&g_requests, 1);
    add_counter(&g_bytes, (uint64_t)bytes * 2u);
}

static void on_tcp_connection(uv_stream_t* server, int status)
{
    if (status < 0) {
        add_counter(&g_errors, 1);
        return;
    }

    TcpClientState* client = (TcpClientState*)calloc(1, sizeof(*client));
    if (client == NULL || uv_tcp_init(server->loop, &client->handle) != 0) {
        free(client);
        add_counter(&g_errors, 1);
        return;
    }
    client->handle.data = client;
    if (uv_accept(server, (uv_stream_t*)&client->handle) != 0 ||
        uv_read_start((uv_stream_t*)&client->handle, alloc_tcp_buffer, on_tcp_read) != 0) {
        add_counter(&g_errors, 1);
        uv_close((uv_handle_t*)&client->handle, close_client);
        return;
    }
    add_counter(&g_connections, 1);
}

static void free_udp_send(uv_udp_send_t* request, int status)
{
    UdpSendState* state = (UdpSendState*)request->data;
    if (status < 0) {
        add_counter(&g_errors, 1);
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
            add_counter(&g_errors, 1);
        }
        return;
    }

    uv_buf_t echo = uv_buf_init(buffer->base, (unsigned int)bytes);
    const int sent = uv_udp_try_send(socket, &echo, 1, peer);
    if (sent == bytes) {
        add_counter(&g_requests, 1);
        add_counter(&g_bytes, (uint64_t)bytes * 2u);
        return;
    }

    UdpSendState* state = (UdpSendState*)calloc(1, sizeof(*state));
    if (state == NULL) {
        add_counter(&g_errors, 1);
        return;
    }
    state->buffer = (char*)malloc((size_t)bytes);
    if (state->buffer == NULL) {
        add_counter(&g_errors, 1);
        free(state);
        return;
    }
    if (memcpy(state->buffer, buffer->base, (size_t)bytes) != state->buffer) {
        add_counter(&g_errors, 1);
        free(state->buffer);
        free(state);
        return;
    }
    echo = uv_buf_init(state->buffer, (unsigned int)bytes);
    state->request.data = state;
    if (uv_udp_send(&state->request, socket, &echo, 1, peer, free_udp_send) != 0) {
        add_counter(&g_errors, 1);
        free(state->buffer);
        free(state);
        return;
    }
    add_counter(&g_requests, 1);
    add_counter(&g_bytes, (uint64_t)bytes * 2u);
}

static void report_stats(uv_timer_t* timer)
{
    (void)timer;
    const uint64_t requests = load_counter(&g_requests);
    const uint64_t bytes = load_counter(&g_bytes);
    const uint64_t errors = load_counter(&g_errors);
    const uint64_t request_delta = requests - g_last_requests;
    const uint64_t byte_delta = bytes - g_last_bytes;
    if (printf("requests_per_sec=%llu throughput_mb_per_sec=%.3f total_requests=%llu errors=%llu\n",
               (unsigned long long)request_delta,
               (double)byte_delta / 1024.0 / 1024.0,
               (unsigned long long)requests,
               (unsigned long long)errors) < 0 ||
        fflush(stdout) != 0) {
        add_counter(&g_errors, 1);
    }
    g_last_requests = requests;
    g_last_bytes = bytes;
}

static void stop_loop(uv_signal_t* signal_handle, int signum)
{
    (void)signal_handle;
    (void)signum;
    for (int i = 0; i < g_tcp_worker_count; ++i) {
        if (g_tcp_workers[i].start_result == 0 &&
            uv_async_send(&g_tcp_workers[i].stopper) != 0) {
            add_counter(&g_errors, 1);
        }
    }
    uv_stop(g_loop);
}

static void stop_tcp_worker(uv_async_t* stopper)
{
    uv_stop(stopper->loop);
}

static void close_worker_handle(uv_handle_t* handle, void* arg)
{
    (void)arg;
    if (uv_is_closing(handle)) {
        return;
    }
    uv_close(handle,
             handle->type == UV_TCP && handle->data != NULL ? close_client : NULL);
}

static int open_reuseport_socket(void)
{
#if defined(SO_REUSEPORT)
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        const int saved_errno = errno;
        if (close(fd) != 0) {
            add_counter(&g_errors, 1);
        }
        errno = saved_errno;
        return -1;
    }
    return fd;
#else
    errno = ENOTSUP;
    return -1;
#endif
}

static void tcp_worker_main(void* arg)
{
    TcpServerWorker* worker = (TcpServerWorker*)arg;
    int result = uv_loop_init(&worker->loop);
    if (result != 0) {
        worker->start_result = result;
        atomic_store_explicit(&worker->ready, 1, memory_order_release);
        return;
    }

    result = uv_tcp_init(&worker->loop, &worker->server);
    int fd = -1;
    if (result == 0) {
        fd = open_reuseport_socket();
        if (fd < 0) {
            result = uv_translate_sys_error(errno);
        }
    }
    if (result == 0) {
        result = uv_tcp_open(&worker->server, fd);
        if (result == 0) {
            fd = -1;
        }
    }
    if (fd >= 0 && close(fd) != 0) {
        add_counter(&g_errors, 1);
    }
    if (result == 0) {
        result = uv_tcp_bind(&worker->server,
                             (const struct sockaddr*)&worker->address,
                             0);
    }
    if (result == 0) {
        result = uv_async_init(&worker->loop, &worker->stopper, stop_tcp_worker);
    }
    if (result == 0) {
        worker->server.data = NULL;
        result = uv_listen((uv_stream_t*)&worker->server, kBacklog, on_tcp_connection);
    }

    worker->start_result = result;
    atomic_store_explicit(&worker->ready, 1, memory_order_release);
    if (result == 0) {
        worker->active_handles = uv_run(&worker->loop, UV_RUN_DEFAULT);
    }

    uv_walk(&worker->loop, close_worker_handle, NULL);
    (void)uv_run(&worker->loop, UV_RUN_DEFAULT);
    const int close_result = uv_loop_close(&worker->loop);
    if (close_result != 0) {
        add_counter(&g_errors, 1);
    }
}

static int workers_ready(const TcpServerWorker* workers, int count)
{
    for (int i = 0; i < count; ++i) {
        if (!atomic_load_explicit(&workers[i].ready, memory_order_acquire)) {
            return 0;
        }
    }
    return 1;
}

static void request_worker_stop(TcpServerWorker* workers, int count)
{
    for (int i = 0; i < count; ++i) {
        if (workers[i].start_result == 0 && uv_async_send(&workers[i].stopper) != 0) {
            add_counter(&g_errors, 1);
        }
    }
}

static int run_tcp_servers(uv_loop_t* control_loop,
                           const struct sockaddr_in* address,
                           int port,
                           int loop_count)
{
    TcpServerWorker* workers = (TcpServerWorker*)calloc((size_t)loop_count, sizeof(*workers));
    if (workers == NULL) {
        return UV_ENOMEM;
    }
    g_tcp_workers = workers;
    g_tcp_worker_count = loop_count;

    int created = 0;
    int result = 0;
    for (; created < loop_count; ++created) {
        workers[created].address = *address;
        atomic_init(&workers[created].ready, 0);
        result = uv_thread_create(&workers[created].thread, tcp_worker_main, &workers[created]);
        if (result != 0) {
            break;
        }
    }

    while (!workers_ready(workers, created)) {
        uv_sleep(1);
    }
    if (result == 0) {
        for (int i = 0; i < created; ++i) {
            if (workers[i].start_result != 0) {
                result = workers[i].start_result;
                break;
            }
        }
    }

    if (result != 0) {
        request_worker_stop(workers, created);
    } else if (printf("meta: implementation=libuv version=%s backend=%s role=server scenario=tcp-echo port=%d loops=%d\n",
                      uv_version_string(), benchmark_backend(), port, loop_count) < 0) {
        result = UV_EIO;
        request_worker_stop(workers, created);
    } else {
        (void)uv_run(control_loop, UV_RUN_DEFAULT);
    }

    int active_handles = 0;
    for (int i = 0; i < created; ++i) {
        const int joined = uv_thread_join(&workers[i].thread);
        if (joined != 0 && result == 0) {
            result = joined;
        }
        active_handles += workers[i].active_handles;
    }
    if (printf("shutdown_active_handles=%d errors=%llu\n",
               active_handles,
               (unsigned long long)load_counter(&g_errors)) < 0 && result == 0) {
        result = UV_EIO;
    }

    g_tcp_workers = NULL;
    g_tcp_worker_count = 0;
    free(workers);
    return result;
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

static int parse_loop_count(const char* text, int* loop_count)
{
    char* end = NULL;
    const long parsed = strtol(text, &end, 10);
    if (text[0] == '\0' || end == NULL || *end != '\0' || parsed < 1 || parsed > kMaxLoops) {
        return 1;
    }
    *loop_count = (int)parsed;
    return 0;
}

int main(int argc, char** argv)
{
    if ((argc != 3 && argc != 4) ||
        (strcmp(argv[1], "tcp") != 0 && strcmp(argv[1], "udp") != 0)) {
        if (fprintf(stderr, "Usage: %s <tcp|udp> <port> [loops]\n", argv[0]) < 0) {
            return 5;
        }
        return 1;
    }
    int port = 0;
    int loop_count = 1;
    if (parse_port(argv[2], &port) != 0 ||
        (argc == 4 && parse_loop_count(argv[3], &loop_count) != 0) ||
        (strcmp(argv[1], "udp") == 0 && loop_count != 1)) {
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
        result = run_tcp_servers(&loop, &address, port, loop_count);
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
                           (unsigned long long)load_counter(&g_errors)) < 0) {
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
