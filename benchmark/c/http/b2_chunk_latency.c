#include <galay/c/galay-http-c/http_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct ChunkBench {
    galay_http_server_t* server;
    C_Host endpoint;
    int iterations;
    int server_ok;
    int client_ok;
    int64_t elapsed_ns;
} ChunkBench;

static int parse_iterations(int argc, char** argv)
{
    if (argc < 2) {
        return 100;
    }
    char* end = NULL;
    long parsed = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || parsed <= 0 || parsed > 10000) {
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

static int assign_loopback(C_Host* host)
{
    host->type = C_IPTypeIPV4;
    int written = snprintf(host->address, sizeof(host->address), "%s", "127.0.0.1");
    if (written <= 0 || (size_t)written >= sizeof(host->address)) {
        return 1;
    }
    host->port = 0;
    return 0;
}

static void server_entry(void* arg)
{
    static const char first[] = "HTTP/1.1 200 OK\r\ncontent-length: 6\r\n\r\nabc";
    static const char second[] = "def";
    ChunkBench* bench = (ChunkBench*)arg;
    for (int i = 0; i < bench->iterations; ++i) {
        galay_http_session_t* session = NULL;
        galay_http_request_t* request = NULL;
        C_IOResult io = galay_http_server_accept(bench->server, &session, NULL, 2000);
        if (io.code == C_IOResultOk) {
            io = galay_http_session_recv_request(session, &request, 4096, 4096, 2000);
        }
        if (request != NULL) {
            galay_http_request_destroy(request);
        }
        if (io.code == C_IOResultOk) {
            io = galay_http_session_send_bytes(session, first, sizeof(first) - 1, 2000);
        }
        if (io.code == C_IOResultOk) {
            C_IOResult yielded = galay_coro_yield();
            if (yielded.code != C_IOResultOk) {
                io = yielded;
            }
        }
        if (io.code == C_IOResultOk) {
            io = galay_http_session_send_bytes(session, second, sizeof(second) - 1, 2000);
        }
        C_IOResult closed = session != NULL
            ? galay_http_session_close(session, 2000)
            : (C_IOResult){C_IOResultInvalid, 0, 0, 0, NULL};
        if (session != NULL && galay_http_session_destroy(session) != GALAY_OK) {
            return;
        }
        if (io.code != C_IOResultOk || closed.code != C_IOResultOk) {
            return;
        }
    }
    bench->server_ok = 1;
}

static void client_entry(void* arg)
{
    ChunkBench* bench = (ChunkBench*)arg;
    int64_t begin = monotonic_ns();
    if (begin < 0) {
        return;
    }
    for (int i = 0; i < bench->iterations; ++i) {
        galay_http_client_t* client = NULL;
        galay_http_request_t* request = NULL;
        galay_http_response_t* response = NULL;
        const char* body = NULL;
        size_t body_len = 0;
        C_IOResult io = {C_IOResultError, 0, 0, 0, NULL};
        if (galay_http_client_create(&client) != GALAY_OK ||
            galay_http_request_create(&request) != GALAY_OK ||
            galay_http_request_set_method_path(request, GALAY_HTTP_METHOD_GET, "/chunk") != GALAY_OK ||
            galay_http_request_set_body(request, NULL, 0) != GALAY_OK) {
            if (response != NULL) {
                galay_http_response_destroy(response);
            }
            if (request != NULL) {
                galay_http_request_destroy(request);
            }
            if (client != NULL) {
                if (galay_http_client_destroy(client) != GALAY_OK) {
                    return;
                }
            }
            return;
        }
        io = galay_http_client_connect(client, &bench->endpoint, 2000);
        if (io.code == C_IOResultOk) {
            io = galay_http_client_send_request(client, request, 2000);
        }
        if (io.code == C_IOResultOk) {
            io = galay_http_client_recv_response(client, &response, 4096, 4096, 2000);
        }
        C_IOResult closed = galay_http_client_close(client, 2000);
        int body_ok = response != NULL &&
            galay_http_response_body(response, &body, &body_len) == GALAY_OK &&
            body_len == strlen("abcdef") &&
            strncmp(body, "abcdef", body_len) == 0;
        if (response != NULL) {
            galay_http_response_destroy(response);
        }
        galay_http_request_destroy(request);
        if (galay_http_client_destroy(client) != GALAY_OK ||
            io.code != C_IOResultOk ||
            body_ok != 1 ||
            (closed.code != C_IOResultOk && closed.code != C_IOResultInvalid)) {
            return;
        }
    }
    int64_t end = monotonic_ns();
    bench->elapsed_ns = end >= begin ? end - begin : -1;
    bench->client_ok = bench->elapsed_ns >= 0 ? 1 : 0;
}

static int cleanup(galay_kernel_runtime_t* runtime,
                   galay_coro_task_t* server_task,
                   galay_coro_task_t* client_task,
                   ChunkBench* bench,
                   int exit_code)
{
    if (server_task->task != NULL && galay_coro_destroy(server_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 10;
    }
    if (client_task->task != NULL && galay_coro_destroy(client_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 11;
    }
    if (bench->server != NULL) {
        C_IOResult stopped = galay_http_server_stop(bench->server, 2000);
        if (stopped.code != C_IOResultOk && stopped.code != C_IOResultInvalid && exit_code == 0) {
            exit_code = 12;
        }
        if (galay_http_server_destroy(bench->server) != GALAY_OK && exit_code == 0) {
            exit_code = 13;
        }
    }
    if (runtime->runtime != NULL) {
        if (galay_kernel_runtime_stop(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 14;
        }
        if (galay_kernel_runtime_destroy(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 15;
        }
    }
    return exit_code;
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
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    ChunkBench bench = {0};
    bench.iterations = iterations;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_http_server_create(&bench.server) != GALAY_OK ||
        assign_loopback(&bench.endpoint) != 0 ||
        galay_http_server_bind(bench.server, &bench.endpoint) != GALAY_OK ||
        galay_http_server_listen(bench.server, 128) != GALAY_OK ||
        galay_http_server_local_endpoint(bench.server, &bench.endpoint) != GALAY_OK) {
        return cleanup(&runtime, &server_task, &client_task, &bench, 3);
    }
    if (galay_coro_spawn(&runtime, server_entry, &bench, NULL, &server_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, client_entry, &bench, NULL, &client_task).code != C_IOResultOk ||
        galay_coro_join(&client_task, 30000).code != C_IOResultOk ||
        galay_coro_join(&server_task, 30000).code != C_IOResultOk ||
        bench.server_ok != 1 ||
        bench.client_ok != 1) {
        return cleanup(&runtime, &server_task, &client_task, &bench, 4);
    }
    double avg_us = (double)bench.elapsed_ns / (double)iterations / 1000.0;
    if (printf("c_http_chunk_latency iterations=%d elapsed_ns=%lld avg_us=%.2f\n",
               iterations, (long long)bench.elapsed_ns, avg_us) < 0) {
        return cleanup(&runtime, &server_task, &client_task, &bench, 5);
    }
    return cleanup(&runtime, &server_task, &client_task, &bench, 0);
}
