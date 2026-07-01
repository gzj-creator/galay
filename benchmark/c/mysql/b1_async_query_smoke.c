#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-mysql-c/mysql_c.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct MysqlAsyncQueryBench {
    galay_kernel_tcp_socket_t* listener;
    C_Host peer;
    galay_kernel_tcp_socket_t accepted;
    int iterations;
    int server_ok;
    int client_ok;
    int64_t elapsed_ns;
    char request[64];
} MysqlAsyncQueryBench;

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

static int send_exact(galay_kernel_tcp_socket_t* socket, const unsigned char* buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        C_IOResult result =
            galay_kernel_tcp_socket_send(socket, (const char*)buffer + sent, length - sent, 1000);
        if (result.code != C_IOResultOk || result.bytes == 0) {
            return 1;
        }
        sent += result.bytes;
    }
    return 0;
}

static void server_entry(void* arg)
{
    static const unsigned char handshake_packet[] = {
        0x05, 0x00, 0x00, 0x00, 'H', 'E', 'L', 'L', 'O'
    };
    static const unsigned char expected_query[] = {
        0x09, 0x00, 0x00, 0x00, 0x03, 'S', 'E', 'L', 'E', 'C', 'T', ' ', '1'
    };
    static const unsigned char ok_packet[] = {0x01, 0x00, 0x00, 0x01, 0x00};
    MysqlAsyncQueryBench* bench = (MysqlAsyncQueryBench*)arg;

    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(bench->listener, &bench->accepted, NULL, 1000);
    if (accepted.code != C_IOResultOk) {
        return;
    }
    if (send_exact(&bench->accepted, handshake_packet, sizeof(handshake_packet)) != 0) {
        return;
    }
    for (int i = 0; i < bench->iterations; ++i) {
        if (recv_exact(&bench->accepted, bench->request, sizeof(expected_query)) != 0 ||
            memcmp(bench->request, expected_query, sizeof(expected_query)) != 0 ||
            send_exact(&bench->accepted, ok_packet, sizeof(ok_packet)) != 0) {
            return;
        }
    }
    C_IOResult closed = galay_kernel_tcp_socket_close(&bench->accepted, 1000);
    bench->server_ok = closed.code == C_IOResultOk ? 1 : 0;
}

static int validate_result_packet(galay_mysql_buffer_t* result_packet)
{
    const unsigned char* packet = NULL;
    size_t packet_len = 0;
    galay_mysql_packet_view_t view = {0};
    return result_packet != NULL &&
        galay_mysql_buffer_data(result_packet, &packet, &packet_len) == GALAY_OK &&
        galay_mysql_extract_packet(packet, packet_len, &view) == GALAY_OK &&
        view.sequence_id == 1 &&
        view.payload_len == 1 &&
        view.payload[0] == 0x00
        ? 0
        : 1;
}

static void client_entry(void* arg)
{
    MysqlAsyncQueryBench* bench = (MysqlAsyncQueryBench*)arg;
    galay_mysql_config_t* config = NULL;
    galay_mysql_client_t* client = NULL;
    int queries_ok = 0;

    if (galay_mysql_config_create(&config) != GALAY_OK ||
        galay_mysql_config_set_host(config, bench->peer.address) != GALAY_OK ||
        galay_mysql_config_set_port(config, bench->peer.port) != GALAY_OK ||
        galay_mysql_client_create(&client) != GALAY_OK ||
        client == NULL) {
        goto cleanup;
    }
    C_IOResult connected = galay_mysql_client_connect_async(client, config, 1000);
    if (connected.code != C_IOResultOk) {
        goto cleanup;
    }

    int64_t begin = monotonic_ns();
    if (begin < 0) {
        bench->elapsed_ns = -1;
        goto close_client;
    }
    for (int i = 0; i < bench->iterations; ++i) {
        galay_mysql_buffer_t* result_packet = NULL;
        C_IOResult queried =
            galay_mysql_client_query_async(client, "SELECT 1", 1000, &result_packet);
        if (queried.code != C_IOResultOk || validate_result_packet(result_packet) != 0) {
            if (result_packet != NULL) {
                galay_mysql_buffer_destroy(result_packet);
            }
            goto close_client;
        }
        galay_mysql_buffer_destroy(result_packet);
    }
    int64_t end = monotonic_ns();
    bench->elapsed_ns = end >= begin ? end - begin : -1;
    queries_ok = bench->elapsed_ns >= 0 ? 1 : 0;

close_client:
    if (client != NULL) {
        C_IOResult closed = galay_mysql_client_close_async(client, 1000);
        bench->client_ok = closed.code == C_IOResultOk && queries_ok == 1 ? 1 : 0;
    }

cleanup:
    if (client != NULL) {
        galay_mysql_client_destroy(client);
    }
    if (config != NULL) {
        galay_mysql_config_destroy(config);
    }
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
    MysqlAsyncQueryBench bench = {0};
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

    double avg_us = (double)bench.elapsed_ns / (double)iterations / 1000.0;
    if (printf("c_mysql_async_query iterations=%d elapsed_ns=%lld avg_us=%.2f\n",
               iterations,
               (long long)bench.elapsed_ns,
               avg_us) < 0) {
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
