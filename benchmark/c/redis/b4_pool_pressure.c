#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-redis-c/redis.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct PoolBench {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    int iterations;
    int server_ok;
    int client_ok;
    int64_t elapsed_ns;
    char request[64];
} PoolBench;

static int parse_iterations(int argc, char** argv)
{
    if (argc < 2) {
        return 100;
    }
    char* end = NULL;
    long parsed = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || parsed <= 0 || parsed > 100000) {
        return -1;
    }
    return (int)parsed;
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
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

static int recv_exact(galay_kernel_tcp_socket_t* socket, char* buffer, size_t length)
{
    size_t received = 0;
    while (received < length) {
        C_IOResult result =
            galay_kernel_tcp_socket_recv(socket, buffer + received, length - received, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        received += result.bytes;
    }
    return 0;
}

static int send_exact(galay_kernel_tcp_socket_t* socket, const char* buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        C_IOResult result =
            galay_kernel_tcp_socket_send(socket, buffer + sent, length - sent, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        sent += result.bytes;
    }
    return 0;
}

static void server_entry(void* arg)
{
    static const char ping_request[] = "*1\r\n$4\r\nPING\r\n";
    static const char response[] = "+PONG\r\n";
    PoolBench* bench = (PoolBench*)arg;

    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(bench->listener, &bench->accepted, NULL, 1000);
    if (accepted.code != C_IOResultOk) {
        return;
    }
    for (int i = 0; i < bench->iterations; ++i) {
        if (recv_exact(&bench->accepted, bench->request, sizeof(ping_request) - 1) != 0 ||
            memcmp(bench->request, ping_request, sizeof(ping_request) - 1) != 0 ||
            send_exact(&bench->accepted, response, sizeof(response) - 1) != 0) {
            return;
        }
    }
    C_IOResult closed = galay_kernel_tcp_socket_close(&bench->accepted, 1000);
    bench->server_ok = closed.code == C_IOResultOk ? 1 : 0;
}

static void client_entry(void* arg)
{
    PoolBench* bench = (PoolBench*)arg;
    galay_redis_pool_t* pool = NULL;
    galay_redis_pool_lease_t* lease = NULL;
    galay_redis_pool_config_t config = {
        .client = {
            .host = bench->peer.address,
            .port = bench->peer.port,
            .username = NULL,
            .password = NULL,
            .db_index = 0,
            .resp_version = 2,
            .connect_timeout_ms = 1000,
        },
        .min_connections = 0,
        .max_connections = 1,
        .initial_connections = 0,
    };

    if (galay_redis_pool_create(&config, &pool) != GALAY_OK) {
        return;
    }
    int64_t begin = monotonic_ns();
    if (begin < 0) {
        galay_redis_pool_destroy(pool);
        return;
    }
    for (int i = 0; i < bench->iterations; ++i) {
        galay_redis_reply_t* reply = NULL;
        const char* value = NULL;
        size_t value_len = 0;
        C_IOResult acquired = galay_redis_pool_acquire(pool, 1000, &lease);
        if (acquired.code != C_IOResultOk) {
            galay_redis_pool_destroy(pool);
            return;
        }
        C_IOResult command = galay_redis_client_command_async(galay_redis_pool_lease_client(lease),
                                                              "PING",
                                                              NULL,
                                                              NULL,
                                                              0,
                                                              1000,
                                                              &reply);
        galay_status_t released = galay_redis_pool_release(pool, lease);
        lease = NULL;
        if (command.code != C_IOResultOk ||
            released != GALAY_OK ||
            galay_redis_reply_string(reply, &value, &value_len) != GALAY_OK ||
            value_len != 4 ||
            memcmp(value, "PONG", value_len) != 0) {
            if (reply != NULL) {
                galay_redis_reply_free(reply);
            }
            galay_redis_pool_destroy(pool);
            return;
        }
        galay_redis_reply_free(reply);
    }
    int64_t end = monotonic_ns();
    bench->elapsed_ns = end >= begin ? end - begin : -1;
    bench->client_ok = bench->elapsed_ns >= 0 ? 1 : 0;
    galay_redis_pool_destroy(pool);
}

int main(int argc, char** argv)
{
    int iterations = parse_iterations(argc, argv);
    if (iterations <= 0) {
        if (fprintf(stderr, "usage: %s [iterations]\n", argv[0]) < 0) {
            return 2;
        }
        return 1;
    }

    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_kernel_tcp_socket_t listener = {0};
    C_Host local = {0};
    PoolBench bench = {0};
    galay_coro_task_t server = {0};
    galay_coro_task_t client = {0};
    int exit_code = 0;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        create_listener(&listener, &local) != 0) {
        exit_code = 3;
        goto cleanup;
    }
    bench.listener = &listener;
    bench.peer = local;
    bench.iterations = iterations;

    if (galay_coro_spawn(&runtime, server_entry, &bench, NULL, &server).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &bench, NULL, &client).code != C_IOResultOk ||
        galay_coro_join(&server, 5000).code != C_IOResultOk ||
        galay_coro_join(&client, 5000).code != C_IOResultOk ||
        bench.server_ok != 1 ||
        bench.client_ok != 1) {
        exit_code = 4;
        goto cleanup;
    }
    if (printf("c_redis_pool_pressure iterations=%d elapsed_ns=%lld\n",
               iterations,
               (long long)bench.elapsed_ns) < 0) {
        exit_code = 5;
    }

cleanup:
    if (server.task != NULL && galay_coro_destroy(&server).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 6;
    }
    if (client.task != NULL && galay_coro_destroy(&client).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 7;
    }
    if (bench.accepted.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&bench.accepted) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 8;
    }
    if (listener.socket != NULL &&
        galay_kernel_tcp_socket_destroy(&listener) != C_TcpSocketSuccess &&
        exit_code == 0) {
        exit_code = 9;
    }
    if (runtime.runtime != NULL) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 10;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 11;
        }
    }
    return exit_code;
}
