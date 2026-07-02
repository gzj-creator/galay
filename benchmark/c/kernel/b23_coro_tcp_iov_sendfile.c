#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
    kDefaultIterations = 1000,
};

typedef struct IovSendfileServer {
    galay_kernel_tcp_socket_t* listener;
    galay_kernel_tcp_socket_t accepted;
    int file_fd;
    int iterations;
    uint64_t errors;
    uint64_t completed;
    char read_a[8];
    char read_b[8];
} IovSendfileServer;

static int64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int create_listener(galay_kernel_tcp_socket_t* listener, C_Host* local)
{
    C_Host bind_host = {C_IPTypeIPV4, "127.0.0.1", 0};
    return galay_kernel_tcp_socket_create(listener, C_IPTypeIPV4) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_bind(listener, &bind_host) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_listen(listener, 16) == C_TcpSocketSuccess &&
        galay_kernel_tcp_socket_local_endpoint(listener, local) == C_TcpSocketSuccess &&
        local->port != 0
        ? 0
        : 1;
}

static int connect_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0) {
        return close(fd) == 0 ? -1 : -2;
    }
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout)) != 0) {
        int close_result = close(fd);
        return close_result == 0 ? -1 : -2;
    }
    return fd;
}

static int send_all_fd(int fd, const char* data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n <= 0) {
            return 1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all_fd(int fd, char* data, size_t length)
{
    size_t received = 0;
    while (received < length) {
        ssize_t n = recv(fd, data + received, length - received, 0);
        if (n <= 0) {
            return 1;
        }
        received += (size_t)n;
    }
    return 0;
}

static void server_entry(void* arg)
{
    IovSendfileServer* server = (IovSendfileServer*)arg;
    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(server->listener, &server->accepted, NULL, 5000);
    if (accepted.code != C_IOResultOk) {
        ++server->errors;
        return;
    }

    for (int i = 0; i < server->iterations; ++i) {
        galay_iovec_t read_iov[2];
        read_iov[0].base = server->read_a;
        read_iov[0].len = 3;
        read_iov[1].base = server->read_b;
        read_iov[1].len = 4;
        C_IOResult read_result =
            galay_kernel_tcp_socket_readv(&server->accepted, read_iov, 2, 1000);
        if (read_result.code != C_IOResultOk || read_result.bytes != 7) {
            ++server->errors;
            break;
        }

        const char write_a[] = "iov-";
        const char write_b[] = "reply";
        galay_iovec_t write_iov[2];
        write_iov[0].base = (void*)write_a;
        write_iov[0].len = sizeof(write_a) - 1;
        write_iov[1].base = (void*)write_b;
        write_iov[1].len = sizeof(write_b) - 1;
        C_IOResult write_result =
            galay_kernel_tcp_socket_writev(&server->accepted, write_iov, 2, 1000);
        if (write_result.code != C_IOResultOk || write_result.bytes != 9) {
            ++server->errors;
            break;
        }

        C_IOResult sendfile_result =
            galay_kernel_tcp_socket_sendfile(&server->accepted, server->file_fd, 6, 9, 1000);
        if (sendfile_result.code != C_IOResultOk || sendfile_result.bytes != 9) {
            ++server->errors;
            break;
        }
        ++server->completed;
    }

    C_IOResult closed = galay_kernel_tcp_socket_close(&server->accepted, 1000);
    if (closed.code != C_IOResultOk) {
        ++server->errors;
    }
}

static int run_client_loop(int fd, int iterations)
{
    char response[32];
    for (int i = 0; i < iterations; ++i) {
        if (send_all_fd(fd, "abc1234", 7) != 0 ||
            recv_all_fd(fd, response, strlen("iov-replyend-slice")) != 0 ||
            memcmp(response, "iov-replyend-slice", strlen("iov-replyend-slice")) != 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    int iterations = kDefaultIterations;
    if (argc == 2) {
        iterations = atoi(argv[1]);
    }
    if (iterations <= 0) {
        return 1;
    }

    char template_path[] = "/tmp/galay-b23-coro-tcp-sendfile-XXXXXX";
    int file_fd = mkstemp(template_path);
    if (file_fd < 0) {
        return 2;
    }
    if (unlink(template_path) != 0) {
        if (close(file_fd) != 0) {
            return 4;
        }
        return 3;
    }
    if (write(file_fd, "file-send-slice-data", 20) != 20) {
        if (close(file_fd) != 0) {
            return 5;
        }
        return 3;
    }

    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;
    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    galay_coro_task_t server_task = {0};
    int client_fd = -1;
    int exit_code = 0;

    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, &local) != 0) {
        exit_code = 4;
        goto cleanup;
    }

    IovSendfileServer server = {0};
    server.listener = &listener;
    server.file_fd = file_fd;
    server.iterations = iterations;
    if (galay_coro_spawn(&runtime, server_entry, &server, 0, &server_task).code != C_IOResultOk) {
        exit_code = 5;
        goto cleanup;
    }

    client_fd = connect_client(local.port);
    if (client_fd < 0) {
        exit_code = 6;
        goto cleanup;
    }

    const int64_t start = now_ns();
    if (run_client_loop(client_fd, iterations) != 0) {
        exit_code = 7;
    }
    const int64_t elapsed = now_ns() - start;
    if (galay_coro_join(&server_task, 10000).code != C_IOResultOk && exit_code == 0) {
        exit_code = 8;
    }
    if ((server.errors != 0 || server.completed != (uint64_t)iterations) && exit_code == 0) {
        exit_code = 9;
    }

    const double seconds = elapsed > 0 ? (double)elapsed / 1000000000.0 : 0.0;
    const double ops_per_sec = seconds > 0.0 ? (double)iterations / seconds : 0.0;
    if (printf("coro_tcp_iov_sendfile iterations=%d elapsed_ms=%.3f ops_per_sec=%.2f completed=%llu errors=%llu\n",
               iterations,
               (double)elapsed / 1000000.0,
               ops_per_sec,
               (unsigned long long)server.completed,
               (unsigned long long)server.errors) < 0 &&
        exit_code == 0) {
        exit_code = 16;
    }

cleanup:
    if (client_fd >= 0) {
        if (close(client_fd) != 0 && exit_code == 0) {
            exit_code = 10;
        }
    }
    if (server_task.task != 0) {
        if (galay_coro_destroy(&server_task).code != C_IOResultOk && exit_code == 0) {
            exit_code = 11;
        }
    }
    if (listener.socket != 0) {
        if (galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
            exit_code == 0) {
            exit_code = 12;
        }
    }
    if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
        exit_code = 13;
    }
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
        exit_code = 14;
    }
    if (close(file_fd) != 0 && exit_code == 0) {
        exit_code = 15;
    }
    return exit_code;
}
